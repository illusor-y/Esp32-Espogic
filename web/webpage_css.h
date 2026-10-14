// webpage_css.h - 从 webpage.h 拆分而来
#ifndef SMARTGLASS_WEBPAGE_CSS_H
#define SMARTGLASS_WEBPAGE_CSS_H

static const char PAGE_CSS[] PROGMEM = R"HTML(
        /* ===== Material You 取色：由种子色 #4fd1c5 生成的中性化色调体系 ===== */
        :root {
            --seed: #4fd1c5;

            /* 中性表面（黑基调） */
            --bg: #0d0e11;
            --surface: #15161b;
            --surface-high: #1d1f26;
            --surface-hover: #24262e;

            --outline: #2e303a;
            --outline-strong: #3c3f4a;

            --text: #eaebf0;
            --text-muted: #a3a5b0;
            --text-dim: #6b6d78;

            /* 主色（teal）与其容器色 */
            --primary: #4fd1c5;
            --on-primary: #02201e;
            --primary-container: #14332f;
            --on-primary-container: #86f4e6;

            --warn: #ffc46b;
            --danger: #ff7a72;

            /* 进度条外框：取主色（teal）的色相、把饱和度降到很低、明度抬高，
               即"带主题色调的高灰度色"。轨道底色是 --surface，与弹窗底色相同，
               不加这圈描边时整个轨道是看不见的（只剩填充条）。 */
            --track-line: #688d89;

            /* 圆角体系 */
            --r-xs: 8px;
            --r-sm: 25px;
            --r-md: 16px;
            --r-lg: 22px;
            --r-xl: 28px;
            --r-pill: 999px;

            /* 线性动效（开/合均线性） */
            --t: .28s linear;

            --shadow: 0 10px 28px rgba(0,0,0,.5);
            --shadow-soft: 0 2px 10px rgba(0,0,0,.35);
        }

        * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
        html { color-scheme: dark; }
        body {
            font-family: "PingFang SC", "HarmonyOS Sans SC", "MiSans", "Noto Sans SC", "Microsoft YaHei", system-ui, sans-serif;
            background: var(--bg);
            color: var(--text);
            line-height: 1.5;
            min-height: 100vh;
            padding: calc(16px + env(safe-area-inset-top)) 16px calc(44px + env(safe-area-inset-bottom));
        }
        ::selection { background: rgba(79,209,197,.28); }
        button, input, select { font-family: inherit; }

        /* 弹窗打开时锁定背景滚动 */
        body.modal-open { overflow: hidden; }

        .container { max-width: 720px; margin: 0 auto; }

        /* ===== 头部 ===== */
        .header { display: flex; align-items: center; justify-content: space-between; padding-bottom: 4px; }
        .brand {
            display: flex; align-items: center; gap: 9px;
            font-size: 12px; letter-spacing: .16em; color: var(--text-muted);
            text-transform: uppercase;
            cursor: pointer; padding: 4px 8px; margin-left: -8px; border-radius: var(--r-xs);
            transition: color var(--t), background var(--t);
        }
        .brand:hover { color: var(--text); background: var(--surface-high); }
        .brand-dot {
            width: 8px; height: 8px; border-radius: 50%;
            background: var(--primary); box-shadow: 0 0 12px var(--primary);
            animation: breathe 2.4s linear infinite;
        }
        @keyframes breathe { 0%,100% { opacity: 1; } 50% { opacity: .4; } }

        .clock { text-align: center; padding: 28px 0 22px; }
        .time-display {
            font-family: "SF Mono", "JetBrains Mono", "Roboto Mono", Consolas, monospace;
            font-size: clamp(44px, 13vw, 64px); font-weight: 300;
            letter-spacing: .01em; font-variant-numeric: tabular-nums; color: var(--text);
        }
        .clock-sub { display: flex; align-items: center; justify-content: center; flex-wrap: wrap; gap: 8px; font-size: 12px; color: var(--text-dim); margin-top: 8px; font-family: "SF Mono", "JetBrains Mono", "Roboto Mono", Consolas, "Courier New", monospace; }
        .net-dot { color: var(--text-dim); }
        .copy-chip {
            letter-spacing: 0;
            color: var(--text-dim);
            padding: 3px 10px;
            border-radius: var(--r-pill);
            cursor: pointer;
            transition: color var(--t), background var(--t);
        }
        .copy-chip:hover { color: var(--on-primary-container); background: rgba(79,209,197,.12); }

        /* ===== 卡片 ===== */
        .card {
            background: var(--surface);
            border: 1px solid var(--outline);
            border-radius: 35px;
            padding: 18px;
            margin-bottom: 16px;
            box-shadow: var(--shadow-soft);
            animation: riseIn .45s linear both;
        }
        .section-label {
            font-size: 11px; letter-spacing: .16em; color: var(--text-dim);
            text-transform: uppercase; margin: 2px 4px 12px;
        }
        .divider { height: 1px; background: var(--outline); margin: 8px 0; }
        .two-col { display: grid; grid-template-columns: 1.4fr 1fr; gap: 16px; align-items: stretch; margin-bottom: 16px; }
        .two-col .card { margin-bottom: 0; }
        @keyframes riseIn { from { opacity: 0; transform: translateY(14px); } to { opacity: 1; transform: translateY(0); } }

        /* ===== 时间设置 ===== */
        .time-inputs { display: flex; align-items: center; justify-content: center; gap: 6px; margin-bottom: 16px; }
        .time-inputs input { width: 62px; text-align: center; font-size: 18px; font-family: monospace; }
        .time-inputs .sep, .tpair .sep { color: var(--text-dim); font-size: 18px; flex: none; }
        .time-actions { display: flex; gap: 10px; justify-content: center; flex-wrap: wrap; }

        /* ===== 手动时间弹窗（时区 + 拨钟）===== */
        /* 自动/手动是两个独立开关（互斥），上下排列；整组与下方「当前」留出间距 */
        .tz-mode-list { margin-bottom: 12px; }
        .tz-mode-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; }
        .tz-mode-row + .tz-mode-row { margin-top: 8px; }
        .tz-mode-row .switch { flex: none; }
        /* 该行显示"当前生效时区"（如「当前：UTC+8:00（自动）」），是随网络定位刷新 /
           用户手动设置而变化的实时值，故用主题色强调，与静态说明文字区分开 */
        .tz-now {
            text-align: center; font-size: 13px; color: var(--primary);
            margin-bottom: 14px; font-variant-numeric: tabular-nums;
        }
        /* AP 模式提示：与 .modal-desc 同规格（12px + 暗色），说明开关为何被锁成手动 */
        .tz-ap-hint { text-align: center; font-size: 12px; color: var(--text-dim); margin-bottom: 12px; }
        .tz-pick { display: flex; align-items: center; justify-content: center; gap: 8px; margin-bottom: 14px; }
        /* 时区下拉复用天气弹窗的 .w-select 自定义下拉：外观与滚动条都已是美化过的。
           宽度逐个指定而非按内容卡死：时放 "-12"/"+14"（3 字符）故略宽，分只放 "00"~"45"（2 字符）故略窄；
           都留出余量，避免胶囊圆角（--r-sm = 25px）下显得像方框。
           展开高度比默认 200px 短，列表照样滚动。 */
        .tz-pick .w-select { flex: none; }
        .tz-pick .w-select[data-ws="tzHour"] { width: 78px; }
        .tz-pick .w-select[data-ws="tzMin"]  { width: 66px; }
        .tz-pick .w-select-btn { padding: 11px 8px; gap: 3px; font-variant-numeric: tabular-nums; }
        .tz-pick .w-select-val { text-align: center; }
        .tz-pick .w-select-arrow { font-size: 10px; }
        .tz-pick .w-select-panel { max-height: 160px; }
        .tz-sep { color: var(--text-dim); font-size: 13px; flex: none; }
        /* 手动时间弹窗的底部按钮靠左下角（其余弹窗仍走全局的右对齐） */
        #manualTimeModal .modal-actions { justify-content: flex-start; }

        /* ===== 全局配置 ===== */
        .row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
        .row-label { font-size: 13px; color: var(--text-muted); }
        .freq-input { width: 110px; border-radius: var(--r-pill); padding: 11px 18px; text-align: center; }
        .freq-display {
            display: inline-flex; align-items: center; justify-content: center; gap: 4px;
            width: 110px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-pill); padding: 11px 18px; font-size: 14px;
            color: var(--text); cursor: pointer; font-variant-numeric: tabular-nums;
            transition: border-color var(--t), background var(--t);
        }
        .freq-display:hover { border-color: var(--primary); background: var(--surface); }
        .unit { color: var(--text-dim); font-size: 13px; }

        /* ===== 输入控件 ===== */
        input, select {
            background: var(--surface-high);
            color: var(--text);
            border: 1px solid var(--outline);
            border-radius: var(--r-sm);
            padding: 11px 12px;
            font-size: 14px;
            transition: border-color var(--t), background var(--t);
        }
        input:focus { outline: none; border-color: var(--primary); background: var(--surface); }
        input::placeholder { color: var(--text-dim); }
        input[type=number]::-webkit-outer-spin-button,
        input[type=number]::-webkit-inner-spin-button { -webkit-appearance: none; margin: 0; }
        input[type=number] { -moz-appearance: textfield; appearance: textfield; }

        /* ===== 按钮 ===== */
        button {
            position: relative; overflow: hidden;
            border: none; border-radius: var(--r-pill); cursor: pointer;
            font-size: 14px; font-weight: 500; padding: 11px 18px;
            transition: transform .16s linear, background var(--t), border-color var(--t), color var(--t), box-shadow var(--t), opacity var(--t);
            -webkit-user-select: none; user-select: none;
        }
        button:active { transform: scale(.96); }
        button:disabled { opacity: .45; cursor: not-allowed; }

        .btn.filled { background: var(--primary); color: var(--on-primary); box-shadow: 0 4px 14px rgba(79,209,197,.22); }
        .btn.filled:hover { filter: brightness(1.06); }
        .btn.tonal { background: var(--surface-high); color: var(--text); border: 1px solid var(--outline); }
        .btn.tonal:hover { background: var(--surface-hover); }
        .btn.danger { background: var(--danger); color: #fff; }
        .btn.danger:hover { filter: brightness(1.08); }

        .icon-btn {
            background: transparent; border: 1px solid var(--outline);
            width: 38px; height: 38px; padding: 0; border-radius: 50%;
            color: var(--text-muted); font-size: 20px; line-height: 1;
        }
        .icon-btn:hover { background: var(--surface-high); }

        /* ===== 复选开关（线性滑动） ===== */
        .switch {
            appearance: none; -webkit-appearance: none;
            position: relative; width: 42px; height: 24px; flex: none;
            background: var(--outline-strong); border-radius: var(--r-pill);
            border: none; cursor: pointer; transition: background var(--t); padding: 0;
        }
        .switch:checked { background: var(--primary); }
        /* 禁用的开关必须看得出禁用：否则与可用态外观一致，用户会反复点。
           与 button:disabled 的 0.45 保持一致。 */
        .switch:disabled { opacity: .45; cursor: not-allowed; }
        .switch::after {
            content: ''; position: absolute; top: 2px; left: 2px; width: 20px; height: 20px;
            background: #fff; border-radius: 50%; transition: transform var(--t);
            box-shadow: 0 1px 3px rgba(0,0,0,.4);
        }
        .switch:checked::after { transform: translateX(18px); }

        .switch-row { display: inline-flex; align-items: center; gap: 10px; }

        /* ===== 分区切换 Tab ===== */
        .section-tabs { display: flex; gap: 8px; margin: 2px 4px 12px; }
        .tab {
            background: transparent; border: 1px solid var(--outline);
            color: var(--text-muted); border-radius: var(--r-pill);
            padding: 8px 18px; font-size: 12px; letter-spacing: .08em;
            transition: background var(--t), color var(--t), border-color var(--t);
        }
        .tab:hover { background: var(--surface-high); }
        .tab.active {
            background: var(--primary-container); color: var(--on-primary-container);
            border-color: transparent;
        }

        /* ===== 序列器条目 ===== */
        .io-entries { display: flex; flex-direction: column; gap: 8px; margin-bottom: 6px; }
        .io-entry {
            display: flex; align-items: center; gap: 12px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 10px 14px; cursor: pointer;
            transition: border-color var(--t), background var(--t);
        }
        .io-entry:hover { background: var(--surface-hover); }
        .io-entry-conflict { border-color: var(--danger); background: rgba(255,122,114,.08); }
        .io-entry-pulse { border-left: 3px solid var(--primary); }
        .io-entry-hb { border-left: 3px solid var(--primary); }
        .io-time { font-family: monospace; font-size: 15px; font-variant-numeric: tabular-nums; }
        .io-time-forever { font-family: inherit; font-size: 13px; color: var(--text-muted); }
        .io-action { font-size: 12px; padding: 3px 11px; border-radius: var(--r-pill); }
        .io-on { background: var(--primary-container); color: var(--on-primary-container); }
        .io-off { background: var(--outline-strong); color: var(--text-muted); }
        .io-mode-tag { font-size: 12px; color: var(--text-muted); }
        .io-hint { font-size: 12px; color: var(--text-muted); margin-left: 10px; }
        .io-conflict-tag { font-size: 11px; color: var(--danger); }
        .io-noperm-tag { font-size: 11px; color: var(--warn); }
        .io-entry-noperm { opacity: .6; }
        .io-del { margin-left: auto; width: 30px; height: 30px; font-size: 14px; }
        .io-empty { text-align: center; color: var(--text-dim); font-size: 13px; padding: 8px 0; }

        /* ===== 调度面板电平配色 ===== */
        .lvl-high { color: var(--primary); font-weight: 600; }
        .lvl-low { color: #9aa0a6; font-weight: 600; }
        .io-panel-name { color: var(--text-muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; min-width: 0; }

        /* ===== 调度面板弹窗：固定高度，仅引脚列表内部滚动（无滚动条） ===== */
        #ioPanelModal .modal {
            max-width: 560px; max-height: 85vh;
            display: flex; flex-direction: column; overflow: hidden;
        }
        #counterPanelModal .modal {
            max-width: 680px; max-height: 85vh;
            display: flex; flex-direction: column; overflow: hidden;
        }
        .io-panel-grid {
            display: grid; grid-template-columns: 1fr 1fr; gap: 8px;
            margin: 4px 0 14px; flex: 1 1 auto; min-height: 0;
            overflow-y: auto; padding-right: 2px;
            scrollbar-width: none; -ms-overflow-style: none;
            overscroll-behavior: contain;
        }
        .io-panel-grid::-webkit-scrollbar { display: none; }
        .io-panel-item {
            display: flex; align-items: center; gap: 8px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 12px 14px; cursor: pointer;
            transition: border-color var(--t), background var(--t);
        }
        .io-panel-item:hover { background: var(--surface-hover); }
        .io-panel-item.disabled { opacity: .5; cursor: not-allowed; }
        .io-panel-item.disabled:hover { background: var(--surface-high); }

        /* ===== 通道卡片 ===== */
        .channel-head { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; margin-bottom: 14px; }
        .channel-title-wrap { display: flex; align-items: center; gap: 9px; flex-wrap: wrap; }
        .channel-dot {
            width: 10px; height: 10px; border-radius: 50%; flex: none;
            background: var(--outline-strong); transition: background var(--t), box-shadow var(--t);
        }
        .channel.on .channel-dot { background: var(--primary); box-shadow: 0 0 10px var(--primary); }
        .channel-head h3 { font-size: 16px; font-weight: 600; }
        .pin-chip {
            display: inline-flex; align-items: center; gap: 6px;
            background: var(--primary-container); color: var(--on-primary-container);
            padding: 6px 13px; font-size: 14px; font-weight: 600;
        }
        .pin-chip:hover { filter: brightness(1.12); }
        .pin-chip .edit-hint { font-size: 10px; font-weight: 400; opacity: .75; }
        .mode-chip {
            display: inline-flex; align-items: center; justify-content: center;
            background: var(--surface-high); color: var(--primary);
            border: 1px solid var(--primary); border-radius: var(--r-pill);
            padding: 3px 11px; font-size: 12px; font-weight: 700; cursor: pointer; flex: none;
        }
        .mode-chip:hover { background: var(--primary-container); color: var(--on-primary-container); }
        .badge {
            font-size: 11px; color: var(--warn); padding: 2px 9px;
            background: rgba(255,196,107,.1); border: 1px solid rgba(255,196,107,.32);
            border-radius: var(--r-pill);
        }
        /* 跨日条目：用主题色（teal）强调，与「阳光」(warn 黄) / 「变轨中」(绿) 拉开区分。
           可点击 → 打开跨日条目说明弹窗 */
        .badge-overnight {
            font-size: 11px; color: var(--primary); padding: 2px 9px;
            background: rgba(79,209,197,.12); border: 1px solid rgba(79,209,197,.4);
            border-radius: var(--r-pill); flex: none;
            cursor: pointer; transition: filter var(--t);
        }
        .badge-overnight:hover { filter: brightness(1.3); }
        /* 变轨中：淡绿色、可点击（点开说明 + 立即完成）。只在输出正在平滑过渡时出现 */
        /* 内含 mm:ss 倒计时，tabular-nums 让数字等宽，避免每秒跳动时徽标宽度抖动 */
        /* 微弱呼吸（复用品牌点的 breathe：1 → .4 → 1）：最暗只是变淡、不会消失，
           目的是"轻微提示这个标签能点"，而不是抢视觉；悬停时停掉呼吸并提亮 */
        .badge-ramp {
            font-size: 11px; color: #6fcf97; padding: 2px 9px;
            background: rgba(111,207,151,.12); border: 1px solid rgba(111,207,151,.4);
            border-radius: var(--r-pill); flex: none; cursor: pointer;
            transition: background var(--t);
            font-variant-numeric: tabular-nums;
            animation: breathe 2.4s linear infinite;
        }
        .badge-ramp:hover { background: rgba(111,207,151,.26); animation: none; }
        .badge-danger {
            font-size: 11px; color: var(--danger); padding: 2px 9px;
            background: rgba(255,122,114,.12); border: 1px solid rgba(255,122,114,.4);
            border-radius: var(--r-pill);
        }
        .channel-actions { display: flex; gap: 8px; align-items: center; }
        .channel-status {
            display: flex; align-items: center; gap: 8px; flex-wrap: wrap;
            background: var(--surface-high); border-radius: var(--r-md);
            padding: 11px 14px; font-size: 13px; color: var(--text-muted); margin-bottom: 14px;
        }
        .channel-status b { color: var(--primary); font-weight: 600; font-variant-numeric: tabular-nums; }
        /* ===== 通道状态标签（原「状态芯片」与「阳光生效」合并后的唯一样式）=====
           颜色即语义，一眼看出"谁在驱动这一路输出"：
             accent = 有条目在跑（含阳光无数据时退回条目）
             warn   = 阳光系列（阳光接管输出中，或阳光已开但当前没有内容在跑）
             danger = 无条目运行（没开阳光，也没有任何东西在跑） */
        .badge-pill {
            font-size: 11px; padding: 2px 9px; border-radius: var(--r-pill);
            border: 1px solid transparent; flex: none; white-space: nowrap;
            cursor: pointer; transition: filter var(--t);
        }
        .badge-pill:hover { filter: brightness(1.3); }
        /* ===== 标签说明弹窗（点状态标签打开）===== */
        /* 说明项较多，比常规弹窗放宽一点；高度沿用 .modal 的滚动策略 */
        #statModal .modal { max-width: 470px; max-height: 86vh; }
        .tag-help-group {
            font-size: 12px; color: var(--text-dim); margin: 16px 0 9px;
            padding-bottom: 5px; border-bottom: 1px solid var(--outline);
        }
        /* 第一个分组紧跟在开头说明之后，不需要那么大的上间距 */
        #statModal .modal-desc + .tag-help-group { margin-top: 4px; }
        .tag-help-row { display: flex; align-items: flex-start; gap: 10px; margin-bottom: 9px; }
        /* 弹窗里的示例标签只做展示：去掉手型（.badge-pill 本体是可点的）*/
        .tag-help-row > span:first-child { flex: none; margin-top: 1px; cursor: default; }
        .tag-help-row .badge-pill:hover { filter: none; }
        .tag-help-text { font-size: 12px; color: var(--text-muted); line-height: 1.55; }
        /* 可点击查看标签说明的标签（点开 statModal）：条目状态、冲突、阳光覆盖 */
        /* 用独立 class 而不是改 .task-state / .badge-danger 本身 —— 那两个是通用样式，
           将来用在纯提示场景时不该长出鼠标手型 */
        .tag-help-click { cursor: pointer; transition: filter var(--t); }
        .tag-help-click:hover { filter: brightness(1.3); }
        .badge-pill.accent {
            color: var(--primary);
            background: rgba(79,209,197,.12); border-color: rgba(79,209,197,.4);
        }
        .badge-pill.warn {
            color: var(--warn);
            background: rgba(255,196,107,.1); border-color: rgba(255,196,107,.32);
        }
        .badge-pill.danger {
            color: var(--danger);
            background: rgba(255,122,114,.12); border-color: rgba(255,122,114,.4);
        }

        /* ===== 条目分组（可折叠，线性动画） ===== */
        .task-group { border: 1px solid var(--outline); border-radius: var(--r-md); margin-bottom: 10px; overflow: hidden; }
        .task-group-header {
            width: 100%; display: flex; align-items: stretch;
            background: transparent; border-radius: 0;
        }
        .task-group-toggle {
            flex: 1; min-width: 0; display: flex; align-items: center; justify-content: space-between;
            background: transparent; border-radius: 0; padding: 9px 12px;
            font-size: 14px; font-weight: 600; color: var(--text);
        }
        .task-group-toggle:hover { background: var(--surface-high); }
        .task-group-toggle .chevron { display: inline-flex; color: var(--text-dim); transition: transform var(--t); }
        .task-del {
            flex: none; display: inline-flex; align-items: center; justify-content: center;
            background: transparent; border-radius: 0; padding: 0 14px;
            border-left: 1px solid var(--outline);
            font-size: 12px; font-weight: 500; color: var(--text-muted);
        }
        .task-del:hover { color: var(--danger); background: rgba(255,122,114,.08); }

        /* ===== 时钟（条目时钟选择 chip + 时钟面板）===== */
        #clockPanelModal .modal { max-width: 480px; }
        .clock-chip {
            display: inline-flex; align-items: center;
            background: var(--primary-container); color: var(--on-primary-container);
            border-radius: var(--r-pill); padding: 4px 11px;
            font-size: 13px; font-weight: 600; font-variant-numeric: tabular-nums;
            cursor: pointer;
        }
        .clock-chip:hover { filter: brightness(1.12); }
        .clk-list { display: flex; flex-direction: column; gap: 8px; margin: 4px 0 14px; }
        .clk-row {
            display: flex; align-items: center; gap: 10px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 12px 14px;
        }
        .clk-chip {
            display: inline-flex; align-items: center;
            background: var(--primary-container); color: var(--on-primary-container);
            border-radius: var(--r-pill); padding: 4px 12px;
            font-size: 14px; font-weight: 600; font-variant-numeric: tabular-nums;
        }
        .clk-used { font-size: 12px; color: var(--warn); }
        .clk-actions { margin-left: auto; display: flex; gap: 8px; }
        .clk-btn { padding: 7px 13px; font-size: 13px; }
        .clk-empty { text-align: center; color: var(--text-dim); font-size: 13px; padding: 12px 0; }
        .taskclock-opt {
            display: block; width: 100%; text-align: left;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 12px 14px;
            font-size: 14px; font-weight: 600; color: var(--text); font-variant-numeric: tabular-nums;
        }
        .taskclock-opt:hover { background: var(--primary-container); color: var(--on-primary-container); }
        .task-header-left { display: flex; align-items: center; gap: 10px; flex: 1; min-width: 0; flex-wrap: wrap; }
        .task-summary { font-size: 12px; color: var(--text-muted); font-family: monospace; font-variant-numeric: tabular-nums; }
        .task-state { font-size: 11px; padding: 2px 9px; border-radius: var(--r-pill); flex: none; }
        .task-state-working { color: #4fd1c5; background: rgba(79,209,197,.12); border: 1px solid rgba(79,209,197,.32); }
        .task-state-idle { color: var(--warn); background: rgba(255,196,107,.1); border: 1px solid rgba(255,196,107,.32); }
        .task-state-disabled { color: var(--text-dim); background: var(--surface); border: 1px solid var(--outline); }
        .task-group-conflict { border-color: var(--danger); }
        .task-group.collapsed .chevron { transform: rotate(-90deg); }
        .task-group-body { max-height: 520px; overflow: hidden; transition: max-height .5s linear, opacity .5s linear; opacity: 1; }
        .task-group.collapsed .task-group-body { max-height: 0; opacity: 0; }
        .task-group-body-inner {
            display: flex; flex-direction: row; flex-wrap: wrap; align-items: center;
            gap: 12px;
            padding: 4px 16px 16px; border-top: 1px solid var(--outline);
        }
        .tcell { display: flex; align-items: center; gap: 7px; }
        .tcell:first-child { flex-basis: 100%; }
        .tcell .label { font-size: 12px; color: var(--text-muted); width: 40px; flex: none; }
        .tpair { display: flex; align-items: center; gap: 6px; flex: none; }
        .tpair input { width: 56px; text-align: center; padding: 9px 6px; font-family: monospace; }
        .arr, .pct { color: var(--text-dim); flex: none; font-size: 13px; }
        .channel-footer { display: flex; justify-content: flex-end; gap: 8px; margin-top: 4px; }

        /* ===== WiFi 悬浮按钮 / 信息 ===== */
        .wifi-fab {
            display: inline-flex; align-items: center; gap: 8px;
            background: var(--surface-high); border: 1px solid var(--outline); color: var(--text);
        }
        .wifi-fab svg { width: 18px; height: 18px; color: var(--primary); flex: none; }
        .wifi-fab span { font-size: 12px; color: var(--text); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 160px; }

        /* ===== 弹窗（线性动画，开/合同速） ===== */
        .modal-mask {
            position: fixed; inset: 0; background: rgba(0,0,0,.55);
            display: flex; align-items: flex-end; justify-content: center; padding: 20px;
            opacity: 0; visibility: hidden; z-index: 999;
            transition: opacity var(--t), visibility var(--t);
            overscroll-behavior: contain;
        }
        .modal-mask.show { opacity: 1; visibility: visible; }
        .modal {
            width: 100%; max-width: 420px; max-height: 82vh; overflow-y: auto;
            background: var(--surface); border: 1px solid var(--outline);
            border-radius: 35px; padding: 20px; box-shadow: var(--shadow);
            transform: translateY(16px) scale(.97); opacity: 0;
            transition: transform var(--t), opacity var(--t);
        }
        #comparatorInputModal .modal { scrollbar-gutter: stable; }
        .modal-mask.show .modal { transform: translateY(0) scale(1); opacity: 1; }
        .modal-head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px; }
        .modal-head h3 { font-size: 18px; }
        .modal-desc { font-size: 12px; color: var(--text-dim); margin-bottom: 14px; }
        .pin-warning { display: none; font-size: 13px; line-height: 1.6; margin-bottom: 12px; }
        .pin-warning .risk-danger { color: var(--danger); }
        .pin-warning .risk-warn { color: var(--warn); }
        .pin-warning .chip-name { color: #3b82f6; }
        .modal-actions { display: flex; gap: 10px; justify-content: flex-end; margin-top: 18px; flex-wrap: wrap; }

        .wifi-list { display: flex; flex-direction: column; gap: 6px; margin: 8px 0 14px; max-height: 220px; overflow-y: auto; }
        .wifi-scan-progress { display: flex; flex-direction: column; gap: 8px; padding: 12px 4px; }
        .wifi-scan-track { height: 6px; border-radius: 4px; overflow: hidden; background: var(--surface); }
        .wifi-scan-bar { height: 100%; width: 0%; background: linear-gradient(90deg, var(--seed), #7fe3d8); border-radius: 4px; transition: width .12s linear; }
        .wifi-scan-text { font-size: 12px; color: var(--text-dim); text-align: center; }
        .wifi-item {
            display: flex; align-items: center; justify-content: space-between; gap: 10px;
            padding: 12px 14px; background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-md); cursor: pointer; transition: background var(--t), border-color var(--t);
        }
        .wifi-item:hover { background: var(--surface-hover); }
        .wifi-item.selected { border-color: var(--primary); background: var(--primary-container); }
        .wifi-item .ssid { font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .wifi-item .meta { font-size: 11px; color: var(--text-dim); flex: none; }

        .pin-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(76px, 1fr)); gap: 8px; margin: 8px 0 4px; }
        .pin-item {
            padding: 9px 6px; font-size: 13px; border-radius: var(--r-sm);
            background: var(--surface-high); border: 1px solid var(--outline); color: var(--text);
        }
        .pin-item:hover { background: var(--surface-hover); }
        .pin-item.current { border-color: var(--primary); background: var(--primary-container); color: var(--on-primary-container); }
        .pin-item:disabled { opacity: .35; cursor: not-allowed; }
        .pin-item.pin-danger { border-color: var(--danger); color: var(--danger); background: rgba(255,122,114,.12); }
        .pin-item.pin-used { opacity: .55; }
        .pin-item.pin-item-block { grid-column: 1 / -1; text-align: left; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

        /* ===== Toast 提示（线性进入/退出） ===== */
        .toast {
            position: fixed; left: 50%; bottom: calc(26px + env(safe-area-inset-bottom)); z-index: 9999;
            transform: translate(-50%, 12px); opacity: 0; visibility: hidden;
            background: var(--surface-high); border: 1px solid var(--outline-strong);
            color: var(--text); border-radius: var(--r-pill); padding: 11px 20px;
            font-size: 14px; max-width: 86vw; text-align: center;
            transition: opacity var(--t), transform var(--t), visibility var(--t);
        }
        .toast.show { opacity: 1; transform: translate(-50%, 0); visibility: visible; }

        /* ===== 保存失败常驻横幅（NVS 写不进去时必须显眼，否则配置静默丢失） ===== */
        .save-fail {
            display: none; align-items: flex-start; gap: 10px;
            margin-top: 14px; padding: 12px 16px;
            background: var(--surface); border: 1px solid var(--danger);
            border-radius: var(--r-md);
        }
        .save-fail.show { display: flex; }
        .save-fail-dot {
            flex: none; width: 9px; height: 9px; margin-top: 5px; border-radius: 50%;
            background: var(--danger); box-shadow: 0 0 10px var(--danger);
        }
        .save-fail-title { font-size: 13px; color: var(--danger); }
        .save-fail-desc { font-size: 12px; color: var(--text-muted); margin-top: 4px; line-height: 1.6; }

        /* ===== 串口日志弹窗（终端风格，保留滚动条回看） ===== */
        .log-modal .modal { max-width: 720px; }
        .log-box {
            background: #090a0d; border: 1px solid var(--outline); border-radius: var(--r-md);
            padding: 14px 16px; font-family: "SF Mono", "JetBrains Mono", "Roboto Mono", Consolas, monospace;
            font-size: 12px; line-height: 1.6; color: #c9ced6;
            height: 52vh; overflow-y: auto; white-space: pre-wrap; word-break: break-all;
            scrollbar-width: thin; scrollbar-color: var(--outline-strong) transparent;
        }
        .log-box::-webkit-scrollbar { width: 6px; }
        .log-box::-webkit-scrollbar-track { background: transparent; margin: 6px 0; }
        .log-box::-webkit-scrollbar-thumb {
            background: var(--outline-strong); border-radius: 999px;
            border: 2px solid transparent; background-clip: padding-box;
        }
        .log-box::-webkit-scrollbar-thumb:hover { background: var(--text-dim); border: 2px solid transparent; background-clip: padding-box; }

        /* ===== NVS 数据弹窗（只读代码框）===== */
        #nvsModal .modal { max-width: 720px; }
        .nvs-box {
            background: #090a0d; border: 1px solid var(--outline); border-radius: var(--r-md);
            padding: 14px 16px; font-family: "SF Mono", "JetBrains Mono", "Roboto Mono", Consolas, monospace;
            font-size: 12px; line-height: 1.6; color: #c9ced6;
            height: 56vh; overflow-y: auto; white-space: pre-wrap; word-break: break-all;
            scrollbar-width: thin; scrollbar-color: var(--outline-strong) transparent;
        }
        .nvs-box::-webkit-scrollbar { width: 6px; }
        .nvs-box::-webkit-scrollbar-track { background: transparent; margin: 6px 0; }
        .nvs-box::-webkit-scrollbar-thumb {
            background: var(--outline-strong); border-radius: 999px;
            border: 2px solid transparent; background-clip: padding-box;
        }
        .nvs-box::-webkit-scrollbar-thumb:hover { background: var(--text-dim); border: 2px solid transparent; background-clip: padding-box; }
        .backup-actions { display: flex; flex-direction: column; gap: 10px; margin-top: 4px; }
        .backup-actions .btn { width: 100%; }

        /* NVS 弹窗标题栏容量条（整块靠右） */
        .nvs-usage { display: flex; align-items: center; gap: 8px; margin-left: auto; margin-right: 10px; }
        .nvs-usage .progress-track { width: 90px; height: 6px; margin: 0; }
        .nvs-usage span { font-size: 11px; color: var(--text-dim); white-space: nowrap; }

        /* ===== 滚动条（页面隐藏 / 弹窗美化） ===== */
        html, body {
            scrollbar-width: none;      /* Firefox：隐藏页面滚动条 */
            -ms-overflow-style: none;   /* IE / 旧 Edge */
        }
        html::-webkit-scrollbar, body::-webkit-scrollbar { display: none; }

        .modal, .wifi-list {
            scrollbar-width: thin;                            /* Firefox：细滚动条 */
            scrollbar-color: var(--outline-strong) transparent;
        }
        .modal::-webkit-scrollbar, .wifi-list::-webkit-scrollbar { width: 6px; height: 6px; }
        .modal::-webkit-scrollbar-button, .wifi-list::-webkit-scrollbar-button { display: none; width: 0; height: 0; }
        .modal::-webkit-scrollbar-corner, .wifi-list::-webkit-scrollbar-corner { background: transparent; }
        .modal::-webkit-scrollbar-track, .wifi-list::-webkit-scrollbar-track {
            background: transparent;
            margin: 6px 0;              /* 上下内缩，让滚动条收敛在圆角内部 */
        }
        .modal::-webkit-scrollbar-thumb, .wifi-list::-webkit-scrollbar-thumb {
            background: var(--outline-strong);
            border-radius: 999px;
            border: 2px solid transparent;    /* 透明描边，胶囊内缩不贴边 */
            background-clip: padding-box;
        }
        .modal::-webkit-scrollbar-thumb:hover, .wifi-list::-webkit-scrollbar-thumb:hover {
            background: var(--text-dim);
            border: 2px solid transparent;
            background-clip: padding-box;
        }

        @media (max-width: 640px) {
            .two-col { grid-template-columns: 1fr; }
        }
        @media (max-width: 480px) {
            body { padding-left: 12px; padding-right: 12px; }
            .tpair input { width: 48px; }
            .wifi-fab span { max-width: 110px; }
        }
        @media (prefers-reduced-motion: reduce) {
            * { animation: none !important; transition: none !important; }
        }

        /* ===== 比较器 ===== */
        .comparator-grid { display: grid; grid-template-columns: 1fr auto 1fr; gap: 14px; align-items: stretch; }
        .comparator-half {
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-md); padding: 14px; min-height: 96px;
        }
        .comparator-half-title {
            display: flex; align-items: center; justify-content: space-between;
            font-size: 11px; letter-spacing: .12em; color: var(--text-dim);
            text-transform: uppercase; margin-bottom: 10px;
        }
        .comparator-items { display: flex; flex-direction: column; gap: 6px; }
        .comparator-item {
            display: flex; align-items: center; gap: 9px;
            background: var(--surface); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 8px 12px; font-size: 13px;
        }
        .comparator-item .comparator-tag {
            font-size: 11px; padding: 2px 9px; border-radius: var(--r-pill);
            background: var(--primary-container); color: var(--on-primary-container); flex: none;
        }
        .comparator-item .comparator-tag.ext { background: var(--outline-strong); color: var(--text-muted); }
        .comparator-item .comparator-ref { color: var(--text-muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .comparator-item .counter-remain { color: var(--primary); font-variant-numeric: tabular-nums; }
        .comparator-item .comparator-del { margin-left: auto; width: 26px; height: 26px; font-size: 13px; flex: none; }
        .comparator-empty { text-align: center; color: var(--text-dim); font-size: 12px; padding: 8px 0; }
        .comparator-bridge {
            display: flex; flex-direction: column; align-items: center; justify-content: center;
            color: var(--text-dim); font-size: 12px; gap: 4px;
        }
        .comparator-bridge .arr { font-size: 22px; }
        .comparator-bridge .bridge-label { color: var(--primary); font-weight: 700; }
        @media (max-width: 520px) { .comparator-grid { gap: 8px; } }

        /* ===== 重置进度条 ===== */
        .progress-track {
            width: 100%; height: 6px; margin: 14px 0 6px;
            background: var(--surface); border-radius: 4px; overflow: hidden;
            /* 用 border 而不是 inset 阴影：填充条会盖住内阴影，导致已填充那段的框线消失 */
            border: 1px solid var(--track-line);
        }
        .progress-bar {
            height: 100%; width: 0%;
            background: linear-gradient(90deg, var(--seed), #7fe3d8);
            border-radius: 4px; transition: width .25s ease;
        }

        /* ===== 中心面板（只读监控） ===== */
        #centralModal .modal { max-width: 680px; }
        .central-section-label {
            font-size: 12px; letter-spacing: .12em; color: var(--primary);
            text-transform: uppercase; margin: 14px 2px 8px; font-weight: 600;
        }
        .central-sub-label { font-size: 11px; color: var(--text-dim); margin: 12px 2px 6px; letter-spacing: .08em; }
        .central-grid {
            display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 4px;
            max-height: 220px; overflow-y: auto; padding-right: 2px;
            scrollbar-width: none; -ms-overflow-style: none;
        }
        .central-grid::-webkit-scrollbar { display: none; }
        .central-pin {
            display: flex; align-items: center; gap: 8px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 10px 12px; cursor: pointer;
            transition: border-color var(--t), background var(--t);
        }
        .central-pin:hover { background: var(--surface-hover); }
        .central-pin .cp-name { color: var(--text-muted); flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 13px; }
        .central-events { display: flex; flex-direction: column; gap: 6px; max-height: 180px; overflow-y: auto; padding-right: 2px; }
        .central-events::-webkit-scrollbar { display: none; }
        .central-events-all { max-height: 55vh; }
        .central-event {
            display: flex; align-items: center; gap: 8px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 9px 12px; cursor: pointer; font-size: 13px;
            transition: border-color var(--t), background var(--t);
        }
        .central-event:hover { background: var(--surface-hover); }
        .central-event .cd-time { color: var(--text-dim); font-size: 11px; margin-left: auto; font-family: monospace; flex: none; }
        .central-arrow { color: var(--text-dim); font-size: 12px; flex: none; }
        .central-empty { text-align: center; color: var(--text-dim); font-size: 12px; padding: 10px 0; }

        /* ===== 中心面板 归因详情 ===== */
        .central-detail-list { display: flex; flex-direction: column; gap: 6px; margin-top: 10px; max-height: 46vh; overflow-y: auto; padding-right: 2px; }
        .central-detail-list::-webkit-scrollbar { display: none; }
        .cd-item {
            display: flex; align-items: flex-start; gap: 10px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 10px 12px;
        }
        .cd-item.cd-active { border-color: var(--primary); background: var(--primary-container); }
        .cd-module {
            flex: none; font-size: 11px; padding: 3px 10px; border-radius: var(--r-pill);
            background: var(--primary-container); color: var(--on-primary-container); height: fit-content;
        }
        .cd-item.cd-active .cd-module { background: var(--primary); color: var(--on-primary); }
        .cd-right { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
        .cd-group { font-size: 13px; font-weight: 600; }
        .cd-entry { font-size: 12px; color: var(--text-muted); word-break: break-all; }
        .cd-item.cd-active .cd-entry { color: var(--on-primary-container); }

        /* ===== 芯片信息与版本固定 ===== */
        /* max-height 的那条覆盖已删除：原本是 #chipModal .modal { max-height: none; }，
           它把全局 .modal 的 82vh 上限覆盖掉，弹窗高度不受约束，加了几行资源数据后直接顶出屏幕。
           现用回全局值；型号列表自身另有 max-height，两者不打架（列表先滚，滚不动了弹窗再滚）。 */
        /* 左右两栏需要更宽：全局 .modal 只有 420px，放不下并排的信息卡与型号列表 */
        #chipModal .modal { max-width: 720px; }
        /* 左=信息卡（定宽，内容长度稳定；320 是让"122KB / 320KB · 最低 96KB"这类值不换行的最小宽度），
           右=型号列表（吃掉剩余宽度）。窄屏改回上下堆叠，与 .two-col 同一套处理。 */
        .chip-cols { display: grid; grid-template-columns: 320px 1fr; gap: 14px; align-items: start; }
        .chip-model-pane { min-width: 0; }      /* 少了这条，长型号名会把网格撑破 */
        @media (max-width: 640px) { .chip-cols { grid-template-columns: 1fr; } }
        .chip-info {
            display: inline-flex; align-items: center; gap: 7px;
            background: var(--surface-high); border: 1px solid var(--outline); color: var(--text);
            padding: 9px 12px; font-size: 12px; white-space: nowrap;
        }
        .chip-info:hover { background: var(--surface-hover); }
        .chip-info svg { width: 15px; height: 15px; color: var(--primary); flex: none; }
        .chip-info-card {
            display: flex; flex-direction: column; gap: 8px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-md); padding: 12px 14px;
        }
        .chip-row { display: flex; align-items: center; justify-content: space-between; gap: 10px; font-size: 13px; color: var(--text-muted); }
        .chip-row b { color: var(--text); font-weight: 600; font-variant-numeric: tabular-nums; }
        .chip-forced-on { color: var(--warn); }
        .chip-forced-off { color: var(--text-muted); }
        /* 改并排后列表可以放高（原 160px）：这段高度本来就被左边的信息卡占着，不额外增加弹窗高度 */
        .chip-model-list { display: flex; flex-direction: column; gap: 6px; max-height: 264px; overflow-y: auto; margin: 0; padding-right: 2px; }
        .chip-model-list::-webkit-scrollbar { display: none; }
        .chip-model-item {
            display: flex; align-items: center; justify-content: space-between; gap: 10px;
            background: var(--surface-high); border: 1px solid var(--outline); border-radius: var(--r-sm);
            padding: 10px 14px; font-size: 14px; color: var(--text); text-align: left;
        }
        .chip-model-item:hover { background: var(--surface-hover); }
        .chip-model-item.selected { border-color: var(--primary); background: var(--primary-container); color: var(--on-primary-container); }
        .chip-model-name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .chip-model-tag { flex: none; font-size: 10px; padding: 2px 8px; border-radius: var(--r-pill); background: var(--primary); color: var(--on-primary); }
        .chip-model-item.selected .chip-model-tag { background: var(--on-primary-container); color: var(--primary-container); }

        /* ===== 天气 ===== */
        /* 弹窗加宽到 640px：省/市/区三项等宽后每项约 196px（原 520px 下省占 1.5 份≈200px、
           市/区各占 1 份≈134px，市与区两级的长名会被省略号截掉）。
           196px 能放下「新疆维吾尔自治区」（8 字）这类最长的省名，
           也给「克孜勒苏柯尔克孜自治州」（11 字）留了余地。 */
        #weatherModal .modal { max-width: 640px; }
        .weather-fab {
            display: inline-flex; align-items: center; gap: 6px;
            background: var(--surface-high); border: 1px solid var(--outline); color: var(--text);
            padding: 9px 12px; font-size: 12px; white-space: nowrap;
        }
        .weather-fab:hover { background: var(--surface-hover); }
        .weather-fab span { font-variant-numeric: tabular-nums; }
        .weather-fab span:first-child { font-size: 15px; }

        /* flex-wrap：窄屏放不下时「接口转发」折到下一行，不会把开关挤变形 */
        .weather-mode-row { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; flex-wrap: wrap; }
        .weather-mode-row .switch { flex: none; }
        /* 「接口转发」推到行最右：margin-left:auto 吃掉左侧剩余空间。
           它放在开关行（而不是下面的 weather-loc 行）是有意的 ——
           传输层兜底与定位方式无关，两种模式下都该在同一位置够得着。 */
        .weather-mode-row .btn { margin-left: auto; flex: none; }
        .weather-loc { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
        .weather-loc .btn { flex: none; }
        /* 三项等宽（不再让省占 1.5 份）；146px 是「新疆维吾尔自治区」8 字 + 内边距 + 箭头的实测需求宽度。
           flex-wrap：窄屏（手机）放不下三项时折行成 2+1，宁可占两行也不把地名截成省略号。 */
        .weather-selects { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 12px; }
        .weather-selects .w-select { flex: 1 1 146px; min-width: 146px; }
        /* 最后一项（区县）的展开列表向左展开，否则宽列表会顶出弹窗右缘 */
        .weather-selects .w-select:last-child .w-select-panel { left: auto; right: 0; }
        /* ===== 自定义下拉（省/市/区）：美化 + 开合动画 + 滚动条 ===== */
        .w-select { position: relative; flex: 1; min-width: 0; }
        .w-select-btn {
            width: 100%; display: flex; align-items: center; justify-content: space-between; gap: 6px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 11px 12px;
            font-size: 13px; font-weight: 400; color: var(--text); text-align: left;
        }
        .w-select-btn:hover { border-color: var(--outline-strong); background: var(--surface-hover); }
        .w-select-btn:active { transform: none; }
        .w-select-val { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .w-select-arrow { flex: none; font-size: 11px; color: var(--text-dim); transition: transform .2s ease; }
        .w-select.open .w-select-arrow { transform: rotate(180deg); }
        .w-select.open .w-select-btn { border-color: var(--primary); }
        /* 展开列表允许比按钮宽：选项名往往比按钮里的选中值更长（区县那一级尤其），
           卡死在按钮宽度上会被省略号切掉。上限 320px / 86vw 保证不越出屏幕。 */
        .w-select-panel {
            position: absolute; left: 0; right: auto; top: calc(100% + 6px); z-index: 30;
            min-width: 100%; width: max-content; max-width: min(320px, 86vw);
            background: var(--surface-high); border: 1px solid var(--outline-strong);
            border-radius: var(--r-sm); box-shadow: var(--shadow);
            padding: 6px; max-height: 200px; overflow-y: auto;
            opacity: 0; visibility: hidden; transform: translateY(-6px);
            transition: opacity .18s ease, transform .18s ease, visibility 0s linear .18s;
            scrollbar-width: thin; scrollbar-color: var(--outline-strong) transparent;
        }
        .w-select.open .w-select-panel {
            opacity: 1; visibility: visible; transform: translateY(0);
            transition: opacity .18s ease, transform .18s ease, visibility 0s linear 0s;
        }
        .w-select-panel::-webkit-scrollbar { width: 5px; }
        .w-select-panel::-webkit-scrollbar-track { background: transparent; }
        .w-select-panel::-webkit-scrollbar-thumb {
            background: var(--outline-strong); border-radius: 999px;
            border: 1px solid transparent; background-clip: padding-box;
        }
        .w-select-panel::-webkit-scrollbar-thumb:hover {
            background: var(--text-dim); border: 1px solid transparent; background-clip: padding-box;
        }
        .w-opt {
            display: block; width: 100%; text-align: left;
            background: transparent; border: none; border-radius: 6px;
            color: var(--text); font-size: 13px; font-weight: 400;
            padding: 8px 10px; cursor: pointer;
            overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
            transition: background var(--t), color var(--t);
        }
        .w-opt:hover { background: var(--surface-hover); }
        .w-opt:active { transform: none; }
        .w-opt.selected { background: var(--primary-container); color: var(--on-primary-container); font-weight: 600; }
        .weather-loc-text { font-size: 13px; color: var(--text-muted); margin-bottom: 4px; word-break: break-all; }
        .weather-loc-debug { display: flex; align-items: baseline; gap: 10px; min-width: 0; font-size: 11px; color: var(--text-dim); flex-wrap: wrap; }
        .weather-loc-debug > span:first-child { min-width: 0; word-break: break-all; }
        .weather-loc-debug > span:last-child { flex: none; white-space: nowrap; font-variant-numeric: tabular-nums; }
        .weather-interact-entry { display: flex; align-items: center; justify-content: space-between; gap: 10px; margin-bottom: 10px; }
        .wi-btn { flex: none; padding: 8px 16px; font-size: 13px; }
        .wi-list { display: flex; flex-direction: column; gap: 8px; margin: 4px 0 14px; }
        .wi-row {
            display: flex; align-items: center; justify-content: space-between; gap: 12px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 10px 12px;
        }
        .wi-pin {
            display: inline-flex; align-items: center;
            background: var(--primary-container); color: var(--on-primary-container);
            border-radius: var(--r-pill); padding: 4px 11px;
            font-size: 13px; font-weight: 600; font-variant-numeric: tabular-nums;
        }
        .wi-empty { text-align: center; color: var(--text-dim); font-size: 13px; padding: 12px 0; }

        /* ===== 大气环境二级弹窗：左右两栏（左气象 / 右阳光）=====
           900px 是按右栏实测需求定的：宽屏下每栏约 423px，去掉引脚标签(62)+设置按钮(46)+开关(42)
           与间隙后，摘要仍有约 210px 可用，放得下最长的一行（"晨光→昏光 05:12→19:36 100%"）。
           窄屏由 .modal 的 width:100% + .modal-mask 的 20px padding 自动收窄，不受此值影响。 */
        .modal-wide { max-width: 900px; }
        .wi-cols { display: flex; gap: 14px; align-items: flex-start; }
        .wi-col { flex: 1 1 0; min-width: 0; }
        .wi-col-title { font-size: 14px; font-weight: 700; margin-bottom: 4px; }
        @media (max-width: 620px) { .wi-cols { flex-direction: column; } }
        /* 大气环境弹窗：顶部公共设置横条（跨两栏的共参放这里，两栏只留对称的通道列表）*/
        .wi-topbar { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
        .wi-topbar-label { font-size: 12px; color: var(--text-muted); flex: none; }
        .wi-topbar input { width: 76px; flex: none; padding: 8px 10px; }
        .wi-topbar-unit { font-size: 12px; color: var(--text-dim); flex: none; }
        .wi-topbar-now { margin-left: auto; font-size: 12px; color: var(--text-muted); font-variant-numeric: tabular-nums; }
        .wi-topbar-hint { font-size: 11px; color: var(--text-dim); line-height: 1.5; margin: 6px 0 14px; }
        /* 两栏的小标题副文本：统一一行，避免两侧标题区高度不等 */
        .wi-col-sub { font-size: 11px; color: var(--text-dim); margin-bottom: 8px; }
        /* 行内统一三段：[引脚][摘要(弹性,可省略)][操作][开关(恒靠最右)] —— 两栏因此天然对齐 */
        .wi-sum {
            flex: 1 1 auto; min-width: 0; font-size: 11px; color: var(--text-dim);
            overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
        }
        .wi-row .switch-row { margin-left: auto; flex: none; }
        .wi-set-btn { flex: none; padding: 6px 12px; font-size: 12px; }

        /* 条目上的「阳光覆盖」淡红标签（阳光接管后条目的时间/占空比不再生效）*/
        .badge-sunoff {
            font-size: 11px; color: #e57373; padding: 2px 9px;
            background: rgba(229,115,115,.12); border: 1px solid rgba(229,115,115,.42);
            border-radius: var(--r-pill); flex: none;
        }
        /* 阳光接管时条目输入置灰（JS 同时设 disabled，这里负责视觉；删除按钮保持可用）*/
        .channel.sun-locked .task-group-body { opacity: .5; }
        .channel.sun-locked .task-group .clock-chip { opacity: .45; }
        .channel.sun-locked .task-group input:disabled { opacity: .55; cursor: not-allowed; }

        /* ===== 阳光交互设置弹窗 ===== */
        .sun-warn { font-size: 12px; line-height: 1.6; color: var(--warn); margin-bottom: 14px; }
        .sun-mode-row { display: flex; gap: 10px; margin-bottom: 10px; flex-wrap: wrap; }
        .sun-mode-btn {
            flex: 1 1 0; min-width: 140px; font-size: 13px; padding: 9px 10px;
            background: var(--surface-high); color: var(--text); border: 1px solid var(--outline);
        }
        .sun-mode-btn.active { background: var(--primary-container); color: var(--on-primary-container); border-color: var(--primary); }
        .sun-times { font-size: 12px; color: var(--text-dim); line-height: 1.6; margin-bottom: 14px; font-variant-numeric: tabular-nums; }
        .sun-times b { color: var(--text); }
        .sun-peak-row { display: flex; align-items: center; gap: 10px; margin-bottom: 6px; }
        .sun-peak-row input { width: 92px; flex: none; }
        .sun-peak-hint { font-size: 11px; color: var(--text-dim); line-height: 1.5; }

        .weather-now {
            display: flex; align-items: center; gap: 16px;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-md); padding: 16px; margin-bottom: 10px;
        }
        .weather-now-icon { font-size: 42px; line-height: 1; }
        .weather-now-main { display: flex; flex-direction: column; }
        .weather-now-temp { font-size: 34px; font-weight: 300; line-height: 1.1; font-variant-numeric: tabular-nums; }
        .weather-now-text { font-size: 14px; color: var(--text-muted); }

        .weather-meta { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
        /* 第二行（日出/日落/晨光/昏光）与上一行留出间距 */
        .weather-meta + .weather-meta { margin-top: 8px; }
        .weather-meta-item {
            display: flex; align-items: center; justify-content: space-between;
            background: var(--surface-high); border: 1px solid var(--outline);
            border-radius: var(--r-sm); padding: 10px 14px; font-size: 13px;
        }
        .weather-meta-item span { color: var(--text-muted); }
        .weather-meta-item b { color: var(--primary); font-weight: 600; font-variant-numeric: tabular-nums; }
    )HTML";

#endif
