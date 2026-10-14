// webpage_js_core.h - 从 webpage.h 拆分而来
#ifndef SMARTGLASS_WEBPAGE_JS_CORE_H
#define SMARTGLASS_WEBPAGE_JS_CORE_H

static const char PAGE_JS_CORE[] PROGMEM = R"HTML(
    'use strict';
    let CONFIG = null;
    let waitingNtp = false;
    let toastTimer = null;

    function $(id){ return document.getElementById(id); }
    function pad2(n){ return (n < 10 ? '0' : '') + n; }
    function fmtTime(h, m, s){ return pad2(h) + ':' + pad2(m) + ':' + pad2(s); }

    function toast(msg){
        clearTimeout(toastTimer);
        const el = $('toast');
        el.textContent = msg;
        el.classList.add('show');
        toastTimer = setTimeout(() => el.classList.remove('show'), 2400);
    }

    function esc(s){
        return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    }

    function val(id, dflt, min, max){
        const v = $(id).value;
        let n = (v === '' || isNaN(v)) ? dflt : Number(v);
        if (typeof min === 'number' && n < min) n = min;
        if (typeof max === 'number' && n > max) n = max;
        return n;
    }

    // 弹窗内容变化时平滑过渡高度。
    // 用 Web Animations API（element.animate）直接驱动高度动画，
    // 不依赖 CSS transition 的起始值记录，从根本上避免「高度直接跳变」的问题。
    function transitionModalHeight(maskEl, apply){
        const inner = maskEl && maskEl.querySelector('.modal');
        if (!inner){ if (apply) apply(); return; }
        const oldH = inner.offsetHeight;
        inner.style.height = oldH + 'px';
        inner.style.overflow = 'hidden';
        if (apply) apply();   // 渲染新内容（高度仍固定 oldH）
        inner.style.height = '0px';   // 临时归零，让 scrollHeight 返回完整内容高度
        const contentH = inner.scrollHeight;
        const newH = Math.min(contentH, Math.round(window.innerHeight * 0.82));   // 与 82vh 对应
        if (Math.abs(newH - oldH) < 2){
            inner.style.height = '';
            inner.style.overflow = '';
            return;
        }
        inner.style.height = oldH + 'px';   // 起点
        const anim = inner.animate(
            [{ height: oldH + 'px' }, { height: newH + 'px' }],
            { duration: 200, easing: 'ease-out' }
        );
        const finish = () => {
            inner.style.height = '';
            inner.style.overflow = '';
        };
        anim.onfinish = finish;
        anim.oncancel = finish;
    }

    // ===== 渲染通道 =====
    function overnight(t){
        return t.enabled && (t.endHour * 60 + t.endMinute) <= (t.startHour * 60 + t.startMinute);
    }

    // 时钟槽位的频率（Hz）；无效返回 0
    function clockFreq(slot){
        const c = (CONFIG && CONFIG.clocks && CONFIG.clocks[slot]) ? CONFIG.clocks[slot] : null;
        return (c && c.enabled) ? c.freq : 0;
    }
    // 时钟标签文字（频率 + hz，不换算 kHz）
    function clockLabel(slot){
        const f = clockFreq(slot);
        return f ? (f + 'hz') : '无时钟';
    }

    // 通道状态标签：原先「状态芯片」和「阳光生效」是两个标签，信息有重叠（阳光接管时一个
    // 说"阳光交互中"、另一个说"阳光生效 x%"），现合并成一个 —— 文案说状态，颜色说"谁在驱动"：
    //   accent(主题色) = 有条目在跑（阳光开着但当前没数据、退回条目时也归这里）
    //   warn(黄)       = 阳光系列（阳光正在接管输出；或阳光已开但此刻没有内容在跑）
    //   danger(红)     = 没东西在跑且与阳光无关（人主动关掉的通道）
    // 百分比不在这里重复：它已经出现在那行公式开头的「阳光原始占空比 X%」里。
    //
    // 穷举（通道关闭时 activeTask 恒为 0，阳光接管时 activeTask 也恒为 0）：
    //   通道 阳光开关 阳光接管 活动条目   文案                      颜色
    //   启用   开      是       —        阳光交互                  黄
    //   启用   开      否      >0        无阳光数据·运行条目X       主题色
    //   启用   开      否       0        无条目运行                 黄
    //   启用   关      —       >0        运行条目X                 主题色
    //   启用   关      —        0        无条目运行                 红
    //   关闭   开      否       0        无条目运行                 黄
    //   关闭   关      —        0        已关闭                     红
    function statusBadgeOf(enabled, sunEnabled, sunActive, activeTask){
        const sunOn = !!(sunEnabled && sunActive);
        const task = activeTask || 0;
        if (sunOn) return { text: '阳光交互', cls: 'warn' };
        if (task > 0) return { text: (sunEnabled ? '无阳光数据·运行条目' : '运行条目') + task, cls: 'accent' };
        // 以下都是"没有任何内容在跑"：与阳光有关就说"无条目运行"（黄），否则区分开关状态
        if (!sunEnabled) {
            return enabled ? { text: '无条目运行', cls: 'danger' }   // 开着但没条目可跑
                           : { text: '已关闭',     cls: 'danger' };  // 人主动关掉的（与阳光无关）
        }
        return { text: '无条目运行', cls: 'warn' };                  // 阳光已开：通道关闭或暂无内容可跑
    }

    function taskSummary(t){
        return pad2(t.startHour) + ':' + pad2(t.startMinute) + '→' + pad2(t.endHour) + ':' + pad2(t.endMinute) + ' · ' + t.startDuty + '%→' + t.endDuty + '%';
    }

    function taskStateOf(t, activeTask, j){
        if (!t.enabled) return { text: '未启用', cls: 'task-state-disabled' };
        if (activeTask === (j + 1)) return { text: '工作中', cls: 'task-state-working' };
        return { text: '待命中', cls: 'task-state-idle' };
    }

    function renderChannel(i, ch){
        const pin = (CONFIG.pwmPins && typeof CONFIG.pwmPins[i] === 'number') ? CONFIG.pwmPins[i] : '?';
        // 阳光"实际接管"标记：条目输入置灰 + 显示「阳光覆盖」都以此为准。
        // 用 sunEnabled && sunActive，而不是只看开关 —— 无太阳时刻数据时阳光会退回条目逻辑，
        // 此刻条目正在生效，若照样置灰会自相矛盾（用户会以为条目没起作用）。
        const sunOn = !!(ch.sunEnabled && ch.sunActive);
        // 状态标签的首帧初值（后端 loadStatus 每 500ms 会按实时状态刷新它）
        const statBadge = statusBadgeOf(ch.enabled, ch.sunEnabled, ch.sunActive, ch.activeTask);
        const hasOvernight = ch.tasks.some(overnight);
        // 「跨日条目」标签：提示该通道有条目跨过午夜。
        // 点击与状态标签一样，都打开同一个标签说明弹窗（statModal），不再单独做弹窗。
        const badge = '<span class="badge-overnight" id="overnight_' + i + '" style="' + (hasOvernight ? '' : 'display:none;') + '" title="点击查看标签说明" onclick="openStatHelp(event)">跨日条目</span>';
        // 「变轨中」标签：输出正在做平滑过渡时才显示（默认隐藏），点开有解释与「立即完成」。
        // 位置紧跟在「跨日条目」右侧。
        const rampBadge = '<span class="badge-ramp" id="ramp_' + i + '" style="display:none;" title="占空比正在平滑过渡，点击查看" onclick="openRampHelp(event)">变轨中</span>';
        let tasksHtml = '';
        const names = ['条目一', '条目二', '条目三', '条目四', '条目五', '条目六'];
        for (let j = 0; j < ch.tasks.length; j++){
            const t = ch.tasks[j];
            const state = taskStateOf(t, ch.activeTask, j);
            const name = names[j] || ('条目' + (j + 1));
            tasksHtml += '' +
            '<div class="task-group collapsed' + (t.conflict ? ' task-group-conflict' : '') + '">' +
                '<div class="task-group-header">' +
                    '<button type="button" class="task-group-toggle" onclick="toggleGroup(this)">' +
                        '<span class="task-header-left">' +
                            '<span class="task-name">' + name + '</span>' +
                            '<span class="task-summary" id="task_summary_' + i + '_' + j + '">' + taskSummary(t) + '</span>' +
                            // 这几个标签都可点击 → 打开标签说明弹窗 statModal。
                            // 它们在 <button class="task-group-header"> 内部，所以必须 stopPropagation
                            // （openStatHelp 里已做），否则点标签会连带把条目折叠/展开。
                            '<span class="task-state tag-help-click ' + state.cls + '" id="task_state_' + i + '_' + j + '" title="点击查看标签说明" onclick="openStatHelp(event)">' + state.text + '</span>' +
                            '<span class="badge-danger tag-help-click" id="task_conflict_' + i + '_' + j + '" style="' + (t.conflict ? '' : 'display:none;') + '" title="点击查看标签说明" onclick="openStatHelp(event)">冲突</span>' +
                            '<span class="badge-sunoff tag-help-click" id="sundis_' + i + '_' + j + '" style="' + (sunOn ? '' : 'display:none;') + '" title="点击查看标签说明" onclick="openStatHelp(event)">阳光覆盖</span>' +
                        '</span>' +
                        '<span class="chevron">▾</span>' +
                    '</button>' +
                    '<button type="button" class="task-del" onclick="pwmTaskDel(' + i + ',' + j + ')" title="删除此条目">删除</button>' +
                '</div>' +
                '<div class="task-group-body">' +
                    '<div class="task-group-body-inner">' +
                        '<div class="tcell">' +
                            '<span class="label">启用</span>' +
                            '<input type="checkbox" class="switch" id="en_' + i + '_' + j + '" ' + (t.enabled ? 'checked' : '') + ' onchange="saveChannel(' + i + ')">' +
                            '<button type="button" class="clock-chip" id="taskclock_' + i + '_' + j + '" onclick="openTaskClock(' + i + ',' + j + ')" title="选择时钟">' + clockLabel(t.clock) + '</button>' +
                        '</div>' +
                        '<div class="tcell">' +
                            '<span class="label">开始</span>' +
                            '<div class="tpair">' +
                                '<input type="number" id="sh_' + i + '_' + j + '" min="0" max="23" value="' + t.startHour + '" onchange="saveChannel(' + i + ')"><span class="sep">:</span>' +
                                '<input type="number" id="sm_' + i + '_' + j + '" min="0" max="59" value="' + t.startMinute + '" onchange="saveChannel(' + i + ')">' +
                            '</div>' +
                        '</div>' +
                        '<div class="tcell">' +
                            '<span class="label">结束</span>' +
                            '<div class="tpair">' +
                                '<input type="number" id="eh_' + i + '_' + j + '" min="0" max="23" value="' + t.endHour + '" onchange="saveChannel(' + i + ')"><span class="sep">:</span>' +
                                '<input type="number" id="em_' + i + '_' + j + '" min="0" max="59" value="' + t.endMinute + '" onchange="saveChannel(' + i + ')">' +
                            '</div>' +
                        '</div>' +
                        '<div class="tcell">' +
                            '<span class="label">占空比</span>' +
                            '<div class="tpair">' +
                                '<input type="number" id="sd_' + i + '_' + j + '" min="0" max="100" value="' + t.startDuty + '" onchange="saveChannel(' + i + ')"><span class="pct">%</span>' +
                                '<span class="arr">→</span>' +
                                '<input type="number" id="ed_' + i + '_' + j + '" min="0" max="100" value="' + t.endDuty + '" onchange="saveChannel(' + i + ')"><span class="pct">%</span>' +
                            '</div>' +
                        '</div>' +
                    '</div>' +
                '</div>' +
            '</div>';
        }
        return '' +
        '<section class="card channel ' + (ch.enabled ? 'on' : '') + (sunOn ? ' sun-locked' : '') + '" id="ch_' + i + '">' +
            '<div class="channel-head">' +
                '<div class="channel-title-wrap">' +
                    '<span class="channel-dot"></span>' +
                    '<button type="button" class="pin-chip" onclick="openPinModal(' + i + ')">GPIO ' + pin + ' <span class="edit-hint">编辑</span></button>' + badge + rampBadge +
                '</div>' +
                '<div class="channel-actions">' +
                    '<button type="button" class="btn tonal" onclick="pwmDel(' + i + ')">删除</button>' +
                    '<button type="button" class="btn filled" onclick="pwmTaskAdd(' + i + ')">添加条目</button>' +
                    '<label class="switch-row" style="cursor:pointer;">' +
                        '<input type="checkbox" class="switch" id="enable_' + i + '" ' + (ch.enabled ? 'checked' : '') + ' onchange="toggleChannel(' + i + ', this)">' +
                    '</label>' +
                '</div>' +
            '</div>' +
            '<div class="channel-status">' +
                '<span><span id="dutyprefix_' + i + '">' + (sunOn ? '阳光原始占空比' : '原始占空比') + '</span> <b id="duty_' + i + '">' + ch.currentDuty.toFixed(3) + '</b>% * ' +
                    '<span id="nointeract_' + i + '" style="' + (ch.weatherInteract ? 'display:none;' : '') + '">无交互</span>' +
                    '<span id="wiinfo_' + i + '" style="' + (ch.weatherInteract ? '' : 'display:none;') + '">大气交互<b id="wfac_' + i + '">--</b></span>' +
                    ' = 实时输出 <b id="wout_' + i + '">' + ch.currentDuty.toFixed(3) + '</b>%</span>' +
                // 状态标签只有一个：文案说状态，颜色说"谁在驱动这一路输出"（见 statusBadgeOf）
                // 可点击 → 打开标签说明弹窗（穷举所有标签及含义）
                '<span class="badge-pill ' + statBadge.cls + '" id="stat_' + i + '" title="点击查看标签说明" onclick="openStatHelp(event)">' + statBadge.text + '</span>' +
            '</div>' +
            tasksHtml +
        '</section>';
    }

    function renderAll(){
        let html = '';
        CONFIG.channels.forEach((ch, i) => { html += renderChannel(i, ch); });
        $('channels').innerHTML = html;
        // 渲染完再统一施加阳光锁：DOM 已就位，遍历一次比在模板串里逐个拼 disabled 更不易漏
        // 判定与 renderChannel 一致：只看"实际接管"（无数据退回条目时条目要可编辑）
        CONFIG.channels.forEach((ch, i) => applySunLock(i, !!(ch.sunEnabled && ch.sunActive)));
    }

    // 阳光接管时：该通道所有条目的"启用开关 / 时间 / 占空比 / 时钟"置灰不可编辑，
    // 并在条目头部显示淡红「阳光覆盖」标签（时间由太阳时刻决定，条目此时不参与计算）。
    // 只禁编辑、不禁删除：条目没被删掉，想清理仍可删；后端此时也不读条目（updatePwmTargetDuty）。
    function applySunLock(i, on){
        const sec = $('ch_' + i);
        if (!sec) return;
        sec.classList.toggle('sun-locked', !!on);
        sec.querySelectorAll('.task-group input, .task-group .clock-chip').forEach(el => { el.disabled = !!on; });
        const tc = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? (CONFIG.channels[i].taskCount || 0) : 0;
        for (let j = 0; j < tc; j++){
            const tag = $('sundis_' + i + '_' + j);
            if (tag) tag.style.display = on ? '' : 'none';
        }
    }

    // ===== 变轨中（天气系数正在平滑过渡）=====
    // 标签本身由 loadStatus 按后端 ramping 字段显隐；这里只管"点开后的解释弹窗"。
    // 只有一个来源（天气现象变化），且系数全局共用 → 「立即完成」是整机动作，无需通道参数。
    // 剩余时间格式 mm:ss（超过 1 小时继续按分钟累加）
    function rampFmt(sec){
        sec = Math.max(0, Math.round(Number(sec) || 0));
        const m = Math.floor(sec / 60), s = sec % 60;
        return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
    }
    // ===== 标签说明弹窗（通道状态标签点开）=====
    // 内容全部写死在 webpage_html.h 的 statModal 里（纯静态说明，不需要请求后端）
    function openStatHelp(e){
        if (e) e.stopPropagation();
        $('statModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeStatHelp(){
        $('statModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }

    // 注：跨日条目等其它提示标签不再各自做弹窗，统一点开上面的 statModal（一个说明入口）。
    // 每个标签的完整解释都写在那一个弹窗里（webpage_html.h 的 statModal）。

    function openRampHelp(e){
        if (e) e.stopPropagation();
        // 过渡时长与「大气环境」弹窗共用同一份配置，这里读一次用于展示（失败就不显示数字）
        fetch('/wxfadeget').then(r => r.json()).then(d => {
            const el = $('rampDurMin');
            if (el && d && d.min != null) el.textContent = d.min;
        }).catch(() => {});
        $('rampModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeRampHelp(){
        $('rampModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function rampComplete(){
        fetchJson('/pwmfadecomplete', { method: 'POST', body: '{}' })
            .then(d => {
                if (d && d.success){
                    toast('已立即完成天气过渡');
                    closeRampHelp();
                    loadStatus();
                } else {
                    toast((d && d.error) || '操作失败');
                }
            })
            .catch(() => {});
    }

    function toggleGroup(el){
        const g = el.closest('.task-group');
        if (g) g.classList.toggle('collapsed');
    }

    // ===== 接口调用 =====
    function fetchJson(url, opts){
        opts = opts || {};
        if (opts.body) opts.headers = { 'Content-Type': 'application/json' };
        return fetch(url, opts).then(r => r.json()).catch(() => { toast('请求失败'); throw new Error('net'); });
    }

    function loadConfig(render){
        return fetchJson('/getconfig').then(cfg => {
            CONFIG = cfg;
            if (render !== false) renderAll();
        });
    }

    // 网络信息（模式/IP/域名）仅在变化时更新，避免轮询重复赋值打断用户的复制选中
    let lastNet = { mode: '', ip: '', host: '' };
    let isApMode = false;
    function updateNetInfo(st){
        isApMode = (st.wifiMode === 'AP');   // 每次轮询实时同步，AP/STA 切换后立即生效
        const mode = (st.wifiMode === 'AP' ? '热点模式' : (st.wifiMode === 'STA' ? 'WiFi 已连接' : 'WiFi 连接中…'));
        const ip = st.ip || '—';
        const host = st.hostname || '—';
        if (mode !== lastNet.mode){
            lastNet.mode = mode;
            const el = $('netMode'); if (el) el.textContent = mode;
        }
        if (ip !== lastNet.ip){
            lastNet.ip = ip;
            const el = $('ipChip'); if (el) el.textContent = ip;
        }
        if (host !== lastNet.host){
            lastNet.host = host;
            const el = $('hostChip'); if (el) el.textContent = host;
        }
    }
    function copyNetInfo(id){
        const el = $(id);
        if (!el) return;
        const txt = (el.textContent || '').trim();
        if (!txt || txt === '—') return;
        const ok = () => toast('已复制：' + txt);
        const legacy = () => {
            const ta = document.createElement('textarea');
            ta.value = txt;
            ta.setAttribute('readonly', '');
            ta.style.position = 'fixed';
            ta.style.opacity = '0';
            document.body.appendChild(ta);
            ta.select();
            try { document.execCommand('copy'); ok(); }
            catch(e){ toast('复制失败，请手动选择复制'); }
            document.body.removeChild(ta);
        };
        if (navigator.clipboard && window.isSecureContext){
            navigator.clipboard.writeText(txt).then(ok).catch(legacy);
        } else {
            legacy();
        }
    }

    // 保存失败常驻横幅：落盘发生在后端 loop 的防抖回调里，HTTP 早已返回成功，
    // 只能靠这个每秒轮询的接口把失败模块名带回来显示；成功后自动收起。
    function renderSaveFail(text){
        const box = $('saveFailBanner');
        if (!box) return;
        if (text){
            $('saveFailText').textContent = text;
            box.classList.add('show');
        } else {
            box.classList.remove('show');
        }
    }

    function loadStatus(){
        return fetchJson('/getstatus').then(st => {
            $('clock').textContent = fmtTime(st.hour, st.minute, st.second);
            updateNetInfo(st);
            renderSaveFail(st.saveFail || '');
            $('wifiInfo').textContent = st.wifiSsid || '连接中…';
            const chipEl = $('chipInfoText');
            if (chipEl){
                const t = (st.chipTemp != null && !isNaN(st.chipTemp)) ? Math.round(st.chipTemp) + '°C' : '—';
                chipEl.textContent = (st.chipModel || '—') + ' · ' + t;
            }
            $('syncNtpBtn').disabled = (st.wifiMode === 'AP');
            (st.channels || []).forEach(c => {
                const duty = $('duty_' + c.index);
                const chEl = $('ch_' + c.index);
                if (duty) duty.textContent = c.currentDuty.toFixed(3);
                // 状态标签（唯一的那个）：文案 + 颜色都由 statusBadgeOf 统一决定
                const stb = $('stat_' + c.index);
                if (stb){
                    const s = statusBadgeOf(c.enabled, c.sunEnabled, c.sunActive, c.activeTask);
                    stb.textContent = s.text;
                    stb.className = 'badge-pill ' + s.cls;
                }
                if (chEl) chEl.classList.toggle('on', c.enabled);
                const enSw = $('enable_' + c.index);
                if (enSw) enSw.checked = c.enabled;
                // 变轨中标签：由后端判定（天气系数过渡未完成），并带剩余时间 mm:ss。
                // 剩余值由后端按 millis() 正计时算出，改时间不影响它；loadStatus 500ms 一轮，秒级倒计时够跟手。
                const rb = $('ramp_' + c.index);
                if (rb){
                    rb.style.display = c.ramping ? '' : 'none';
                    if (c.ramping) rb.textContent = '变轨中 ' + rampFmt(c.rampRemain);
                }
                const noInt = $('nointeract_' + c.index);
                const wiInfo = $('wiinfo_' + c.index);
                if (noInt) noInt.style.display = c.weatherInteract ? 'none' : '';
                if (wiInfo) wiInfo.style.display = c.weatherInteract ? '' : 'none';
                const out = $('wout_' + c.index);
                if (out){
                    const v = c.weatherInteract ? c.weatherDuty : c.currentDuty;
                    out.textContent = ((v != null && !isNaN(v)) ? v : 0).toFixed(3);
                }
                if (c.weatherInteract){
                    const wf = $('wfac_' + c.index);
                    if (wf){
                        const f = (st.weatherFactor != null && !isNaN(st.weatherFactor)) ? st.weatherFactor : 100;
                        wf.textContent = f + '%(' + (WEATHER.text || '--') + (WEATHER.icon || '') + ')';
                    }
                }
                // 阳光交互：只有"实际接管"才禁用条目并显示淡红标签；无数据退回条目时条目必须可编辑
                // （与整页渲染共用 applySunLock，避免两套逻辑打架）
                const sunOn = !!(c.sunEnabled && c.sunActive);
                applySunLock(c.index, sunOn);
                const dp = $('dutyprefix_' + c.index);
                if (dp) dp.textContent = sunOn ? '阳光原始占空比' : '原始占空比';
                const tc = (CONFIG && CONFIG.channels && CONFIG.channels[c.index]) ? (CONFIG.channels[c.index].taskCount || 0) : 0;
                for (let j = 0; j < tc; j++){
                    const t = (CONFIG && CONFIG.channels && CONFIG.channels[c.index]) ? CONFIG.channels[c.index].tasks[j] : null;
                    const stateEl = $('task_state_' + c.index + '_' + j);
                    if (stateEl && t){
                        const s = taskStateOf(t, c.activeTask, j);
                        stateEl.textContent = s.text;
                        // 必须带上 tag-help-click（可点击样式），否则每轮刷新会把 class 冲掉
                        stateEl.className = 'task-state tag-help-click ' + s.cls;
                    }
                }
            });
            // 「大气环境」弹窗开着时同步"活的"数据：顶部「当前」与左栏摘要都基于天气系数与实时占空比，
            // 而 CONFIG 是打开弹窗那一刻的快照。只改文本、不重建 DOM，避免打断用户正在点的开关。
            if (CONFIG && st.weatherFactor != null) CONFIG.weatherFactor = st.weatherFactor;
            const wiModal = $('weatherInteractModal');
            if (wiModal && wiModal.classList.contains('show')){
                renderWiNow();
                const live = st.channels || [];
                for (let k = 0; k < live.length; k++){
                    const ch = (CONFIG && CONFIG.channels) ? CONFIG.channels[live[k].index] : null;
                    if (ch && typeof live[k].currentDuty === 'number') ch.currentDuty = live[k].currentDuty;
                    updateWiSum(live[k].index);
                }
            }
            if (waitingNtp && st.manualFinished){
                waitingNtp = false;
                toast(st.manualSuccess ? '网络时间同步成功' : (st.manualError || '同步失败'));
            }
        });
    }

    function setTime(){
        const h = val('setHour', 0, 0, 23), m = val('setMinute', 0, 0, 59), s = val('setSecond', 0, 0, 59);
        fetchJson('/settime', { method: 'POST', body: JSON.stringify({ hour: h, minute: m, second: s }) })
            .then(() => toast('时间已设置'));
    }

    function syncNTP(){
        const btn = $('syncNtpBtn');
        btn.disabled = true;
        fetchJson('/syncntptime').then(d => {
            if (d.error){ toast(d.error); btn.disabled = false; return; }
            waitingNtp = true;
            toast('正在同步…');
        });
    }

    // ===== 时区（「手动时间」二级弹窗）=====
    // 一律使用"冬令时"（标准时）基准：偏移全年不变，设备时钟不随夏令时跳变。
    // 自动 = 靠网络定位取偏移；手动 = 用户按 UTC±X 指定。
    let TZ = { mode: 'auto', savedMode: 'auto', offset: 0, current: 0, pickH: 0, pickM: 0 };

    function tzSecToString(sec){
        const a = Math.abs(sec);
        return 'UTC' + (sec < 0 ? '-' : '+') + Math.floor(a / 3600) + ':' + pad2(Math.round((a % 3600) / 60));
    }
    // 下拉选择 -> 偏移秒数。分跟着"时"的符号：-3 时 30 分 = UTC-3:30（纽芬兰）
    function tzJoin(h, m){ return h * 3600 + (h < 0 ? -1 : 1) * m * 60; }
    function tzSplit(sec){
        const a = Math.abs(sec);
        return { h: sec < 0 ? -Math.floor(a / 3600) : Math.floor(a / 3600), m: Math.round((a % 3600) / 60) };
    }

    // 时 -12~+14（真实时区边界）；分 0/15/30/45（0 必须保留：整数小时占绝大多数）
    // 复用天气弹窗的自定义下拉（.w-select）：外观与滚动条都已美化，这里只负责填选项与回填显示。
    function tzSetSel(key, code, text){
        const root = document.querySelector('.w-select[data-ws="' + key + '"]');
        if (!root) return;
        const val = root.querySelector('.w-select-val');
        if (val) val.textContent = text;
        root.querySelectorAll('.w-opt').forEach(o => {
            o.classList.toggle('selected', o.getAttribute('data-code') === String(code));
        });
    }

    function tzBuildPanels(){
        const hp = document.querySelector('.w-select[data-ws="tzHour"] .w-select-panel');
        const mp = document.querySelector('.w-select[data-ws="tzMin"] .w-select-panel');
        if (!hp || !mp || hp.childElementCount) return;
        let h = '';
        for (let i = -12; i <= 14; i++) h += '<button type="button" class="w-opt" data-code="' + i + '">' + (i >= 0 ? '+' + i : i) + '</button>';
        hp.innerHTML = h;
        mp.innerHTML = ['00', '15', '30', '45'].map((t, i) => '<button type="button" class="w-opt" data-code="' + (i * 15) + '">' + t + '</button>').join('');
    }

    // 由全局下拉派发（webpage_js_weather.h 的 wsPick）回调：记录当前选择
    function tzSetHour(code){
        TZ.pickH = parseInt(code, 10) || 0;
        tzSetSel('tzHour', TZ.pickH, TZ.pickH >= 0 ? '+' + TZ.pickH : String(TZ.pickH));
    }
    function tzSetMin(code){
        TZ.pickM = parseInt(code, 10) || 0;
        tzSetSel('tzMin', TZ.pickM, pad2(TZ.pickM));
    }

    function tzRender(){
        const autoSw = $('tzAutoSwitch');
        const manSw  = $('tzManualSwitch');
        const now    = $('tzNowText');
        const apHint = $('tzApHint');
        const row    = $('tzManualRow');

        // AP 模式：时间只能靠「手动拨钟」直接设定本地钟点，时区整块不适用 ——
        // 此时 lastSyncEpoch 恒为 0，updateTime 走的是"直接自增时分秒"的分支，
        // 没有 epoch 也就没有"加时区偏移"这一步，设了也不参与任何计算。
        // 故两个开关都关闭且禁止开启，当前值/UTC 选择器一并隐藏。
        if (isApMode){
            if (autoSw){ autoSw.checked = false; autoSw.disabled = true; }
            if (manSw) { manSw.checked  = false; manSw.disabled  = true; }
            if (now)    now.style.display    = 'none';
            if (row)    row.style.display    = 'none';
            if (apHint) apHint.style.display = '';
            return;
        }

        const manual = (TZ.mode === 'manual');
        // 两个独立开关互斥：同时只会亮一个。不用单个开关来回拨，是因为"拨到自动"看起来
        // 像是把手动关掉了、而不是选中了自动，容易误读。
        if (autoSw){ autoSw.checked = !manual; autoSw.disabled = false; }
        if (manSw) { manSw.checked  = manual;  manSw.disabled  = false; }
        if (now)    now.style.display    = '';
        if (apHint) apHint.style.display = 'none';
        if (now) now.textContent = '当前：' + tzSecToString(TZ.current) + (manual ? '（手动）' : '（自动）');
        if (row) row.style.display = manual ? 'block' : 'none';
        const sp = tzSplit(TZ.offset);
        TZ.pickH = sp.h;
        TZ.pickM = sp.m;
        tzSetSel('tzHour', sp.h, sp.h >= 0 ? '+' + sp.h : String(sp.h));
        tzSetSel('tzMin', sp.m, pad2(sp.m));
    }

    function tzLoad(){
        return fetchJson('/gettz').then(d => {
            TZ.mode = (d && d.mode === 'manual') ? 'manual' : 'auto';
            if (d && typeof d.offset === 'number')  TZ.offset  = d.offset;
            if (d && typeof d.current === 'number') TZ.current = d.current;
            TZ.savedMode = TZ.mode;             // 作为"未提交就关弹窗"的回滚基准
            tzRender();
        }).catch(() => {});
    }

    function tzCommit(mode, offset, msg){
        fetchJson('/savetz', { method: 'POST', body: JSON.stringify({ mode: mode, offset: offset }) })
            .then(d => {
                if (d && d.error){ toast(d.error); tzLoad(); return; }
                TZ.savedMode = mode;
                toast(msg);
                tzLoad();
            });
    }

    function openManualTime(){
        // 拨钟输入框每次打开都留空（占位符 "00" 提示格式）：
        // 留空时 val() 会回退到默认 0，点「应用时间」等价于 00:00:00，与占位提示一致。
        $('setHour').value = '';
        $('setMinute').value = '';
        $('setSecond').value = '';
        tzBuildPanels();
        tzLoad();
        $('manualTimeModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function closeManualTime(){
        $('manualTimeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        wsCloseAll();   // 关弹窗时收起可能还开着的时区下拉
        // 拨到手动但没点「修改」就关闭：回滚界面，避免显示与已保存值不一致
        if (TZ.mode !== TZ.savedMode){
            TZ.mode = TZ.savedMode;
            tzRender();
        }
    }

    // 两个开关互斥：打开一个即选中该模式，另一个自动熄灭。
    // 切到手动只改界面（点「应用」才提交）；切到自动立即提交并触发重新定位。
    function tzModeToggle(sw){
        // AP 模式时区整块禁用（开关已 disabled，这里兜底）
        if (isApMode){ tzRender(); return; }
        const manual = (sw.id === 'tzManualSwitch');
        TZ.mode = manual ? 'manual' : 'auto';
        tzRender();
        if (!manual){
            tzCommit('auto', TZ.offset, '已切换为自动时区，正在重新定位…');
        }
    }

    function tzSaveManual(){
        const off = tzJoin(TZ.pickH, TZ.pickM);
        TZ.mode = 'manual';
        TZ.offset = off;
        tzRender();
        tzCommit('manual', off, '时区已设为 ' + tzSecToString(off));
    }

    function toggleChannel(i, sw){
        fetchJson('/togglechannel?channel=' + i).then(d => {
            sw.checked = d.enabled;
            loadStatus();
        });
    }

    function saveChannel(i){
        const count = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? (CONFIG.channels[i].taskCount || 0) : 0;
        const tasks = [];
        for (let j = 0; j < count; j++){
            const cur = (CONFIG.channels[i].tasks && CONFIG.channels[i].tasks[j]) ? CONFIG.channels[i].tasks[j] : null;
            tasks.push({
                enabled: $('en_' + i + '_' + j).checked,
                startHour: val('sh_' + i + '_' + j, 0, 0, 23),
                startMinute: val('sm_' + i + '_' + j, 0, 0, 59),
                endHour: val('eh_' + i + '_' + j, 23, 0, 23),
                endMinute: val('em_' + i + '_' + j, 0, 0, 59),
                startDuty: val('sd_' + i + '_' + j, 0, 0, 100),
                endDuty: val('ed_' + i + '_' + j, 100, 0, 100),
                clock: (cur && cur.clock != null) ? cur.clock : 0
            });
        }
        fetchJson('/savechannel?channel=' + i, { method: 'POST', body: JSON.stringify({ tasks: tasks }) })
            .then(d => {
                if (d.success){
                    // 就地更新前端缓存，避免整页重渲染导致闪烁
                    if (CONFIG && CONFIG.channels && CONFIG.channels[i]){
                        CONFIG.channels[i].tasks = tasks;
                        (d.conflicts || []).forEach((c, j) => {
                            if (CONFIG.channels[i].tasks[j]) CONFIG.channels[i].tasks[j].conflict = !!c;
                        });
                    }
                    updateOvernightBadge(i);
                    updateTaskStates(i);
                    updateTaskConflicts(i);
                    toast('条目配置已保存');
                } else {
                    toast(d.error || '保存失败');
                }
            })
            .catch(() => {});
    }

    // 就地更新跨日条目徽章（不重渲染）
    function updateOvernightBadge(i){
        const badge = $('overnight_' + i);
        if (!badge) return;
        const tasks = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? CONFIG.channels[i].tasks : null;
        if (!tasks) return;
        const has = tasks.some(overnight);
        badge.style.display = has ? '' : 'none';
    }

    // 就地更新条目状态标签与简述（工作中/待命中/未启用）
    function updateTaskStates(i){
        const ch = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? CONFIG.channels[i] : null;
        if (!ch) return;
        for (let j = 0; j < (ch.taskCount || 0); j++){
            if (!ch.tasks[j]) continue;
            const stateEl = $('task_state_' + i + '_' + j);
            const summaryEl = $('task_summary_' + i + '_' + j);
            if (stateEl){
                const s = taskStateOf(ch.tasks[j], ch.activeTask, j);
                stateEl.textContent = s.text;
                stateEl.className = 'task-state ' + s.cls;
            }
            if (summaryEl) summaryEl.textContent = taskSummary(ch.tasks[j]);
        }
    }

    // 就地更新条目冲突标记（红色标签 + 边框）
    function updateTaskConflicts(i){
        const ch = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? CONFIG.channels[i] : null;
        if (!ch) return;
        for (let j = 0; j < (ch.taskCount || 0); j++){
            const badgeEl = $('task_conflict_' + i + '_' + j);
            const t = ch.tasks[j];
            const c = !!(t && t.conflict);
            if (badgeEl){
                badgeEl.style.display = c ? '' : 'none';
                const groupEl = badgeEl.closest('.task-group');
                if (groupEl) groupEl.classList.toggle('task-group-conflict', c);
            }
        }
    }

    // ===== 时钟面板（全局时钟管理，对应 4 个硬件 timer）=====
    function clockUsedSlots(){
        const used = {};
        (CONFIG.channels || []).forEach(ch => {
            (ch.tasks || []).forEach(t => { if (t.clock != null) used[t.clock] = true; });
        });
        return used;
    }
    function openClockPanel(){
        renderClockPanel();
        $('clockPanelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeClockPanel(){
        $('clockPanelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function renderClockPanel(){
        const box = $('clockList');
        if (!box) return;
        const clks = (CONFIG && CONFIG.clocks) ? CONFIG.clocks : [];
        const used = clockUsedSlots();
        let html = '';
        for (let i = 0; i < clks.length; i++){
            const c = clks[i];
            if (!c || !c.enabled) continue;
            const isUsed = !!used[i];
            html += '<div class="clk-row">' +
                '<span class="clk-chip">' + c.freq + 'hz</span>' +
                (isUsed ? '<span class="clk-used">使用中</span>' : '') +
                '<span class="clk-actions">' +
                    '<button type="button" class="btn tonal clk-btn" onclick="clockSetFreqOpen(' + i + ')">改频</button>' +
                    '<button type="button" class="btn danger clk-btn" onclick="clockRemove(' + i + ')" ' + (isUsed ? 'disabled' : '') + '>删除</button>' +
                '</span>' +
            '</div>';
        }
        if (!html) html = '<div class="clk-empty">暂无时钟</div>';
        box.innerHTML = html;
    }
    function clockAddOpen(){
        $('clockFreqInput').value = '';
        $('clockAddModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function clockAddClose(){
        $('clockAddModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function clockAddConfirm(){
        const f = val('clockFreqInput', 20000, 1, 40000);
        fetchJson('/clockadd', { method: 'POST', body: JSON.stringify({ freq: f }) })
            .then(d => {
                if (d.success){ toast('时钟已添加'); clockAddClose(); loadConfig(false).then(() => renderClockPanel()); }
                else toast(d.error || '添加失败');
            })
            .catch(() => {});
    }
    function clockRemove(slot){
        fetchJson('/clockremove', { method: 'POST', body: JSON.stringify({ slot: slot }) })
            .then(d => {
                if (d.success){ toast('时钟已删除'); loadConfig(false).then(() => renderClockPanel()); }
                else toast(d.error || '删除失败');
            })
            .catch(() => {});
    }
    function clockSetFreqOpen(slot){
        const c = (CONFIG.clocks && CONFIG.clocks[slot]) ? CONFIG.clocks[slot] : null;
        $('clockEditSlot').value = slot;
        $('clockEditFreqInput').value = (c && c.freq) ? c.freq : 20000;
        $('clockEditModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function clockEditClose(){
        $('clockEditModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function clockEditConfirm(){
        const slot = parseInt($('clockEditSlot').value, 10);
        const f = val('clockEditFreqInput', 20000, 1, 40000);
        fetchJson('/clocksetfreq', { method: 'POST', body: JSON.stringify({ slot: slot, freq: f }) })
            .then(d => {
                if (d.success){ toast('频率已修改'); clockEditClose(); loadConfig(false).then(() => renderClockPanel()); }
                else toast(d.error || '修改失败');
            })
            .catch(() => {});
    }

    // ===== 条目时钟选择（给某条目选时钟）=====
    let taskClockTarget = { ch: -1, idx: -1 };
    function openTaskClock(ch, idx){
        taskClockTarget = { ch: ch, idx: idx };
        renderTaskClock();
        $('taskClockModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeTaskClock(){
        $('taskClockModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        taskClockTarget = { ch: -1, idx: -1 };
    }
    function renderTaskClock(){
        const box = $('taskClockList');
        if (!box) return;
        const clks = (CONFIG && CONFIG.clocks) ? CONFIG.clocks : [];
        let html = '';
        for (let i = 0; i < clks.length; i++){
            const c = clks[i];
            if (!c || !c.enabled) continue;
            html += '<button type="button" class="taskclock-opt" onclick="taskClockSelect(' + i + ')">' + c.freq + 'hz</button>';
        }
        if (!html) html = '<div class="clk-empty">暂无可用时钟，请先到时钟面板添加。</div>';
        box.innerHTML = html;
    }
    function taskClockSelect(slot){
        const t = taskClockTarget;
        if (t.ch < 0 || t.idx < 0) return;
        if (CONFIG && CONFIG.channels && CONFIG.channels[t.ch] && CONFIG.channels[t.ch].tasks[t.idx]){
            CONFIG.channels[t.ch].tasks[t.idx].clock = slot;
        }
        closeTaskClock();
        saveChannel(t.ch);
        const chip = $('taskclock_' + t.ch + '_' + t.idx);
        if (chip) chip.textContent = clockLabel(slot);
    }

    function openBackupModal(){
        $('backupModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeBackupModal(){
        $('backupModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function openNvsModal(){
        closeBackupModal();
        $('nvsModal').classList.add('show');
        document.body.classList.add('modal-open');
        loadNvsUsage();
        loadNvsData();
    }
    function closeNvsModal(){
        $('nvsModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function loadNvsUsage(){
        const bar = $('nvsUsageBar'), txt = $('nvsUsageText');
        if (bar) bar.style.width = '0%';
        if (txt) txt.textContent = '剩余空间 --';
        fetch('/nvsstats')
            .then(r => r.json())
            .then(d => {
                const used = Math.max(0, Math.min(100, Number(d.percent) || 0));
                if (bar) bar.style.width = used + '%';
                if (txt) txt.textContent = '剩余空间' + (100 - used) + '%';
            })
            .catch(() => { if (txt) txt.textContent = '剩余空间 --'; });
    }
    function loadNvsData(){
        const box = $('nvsBox');
        if (box) box.textContent = '加载中…';
        fetch('/nvsdata')
            .then(r => r.text())
            .then(txt => { if ($('nvsBox')) $('nvsBox').textContent = txt; })
            .catch(() => { if ($('nvsBox')) $('nvsBox').textContent = '加载失败，请重试'; });
    }

    function openExportModal(){
        $('exportModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeExportModal(){
        $('exportModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function doExport(includeDev){
        closeExportModal();
        window.location.href = includeDev ? '/exportconfig?dev=1' : '/exportconfig';
    }
    function exportConfig(){ openExportModal(); }

    function importConfig(input){
        const f = input.files && input.files[0];
        if (!f) return;
        const reader = new FileReader();
        reader.onload = () => {
            fetchJson('/importconfig', { method: 'POST', body: reader.result }).then(d => {
                if (d.success){
                    // 后端在配置含总线（开发模式）引脚项时会带回 warn，如实告诉用户这部分不会持久化
                    toast(d.warn ? ('配置导入成功，设备正在重启…（' + d.warn + '）') : '配置导入成功，设备正在重启…');
                    startAutoReload();
                } else toast(d.error || '配置导入失败');
            });
        };
        reader.readAsText(f);
        input.value = '';
    }

    // ===== GPIO 引脚自定义 =====
    let editingChannel = -1;
    let pendingPins = null;
    let pwmDevMode = false;     // 开发模式：放开 Flash 禁用（改绑引脚）
    let pwmAllowDup = false;    // 允许重复选中引脚（改绑引脚）

    // 引脚风险反查表：按“影响工作”程度分为两级——
    //   danger（红）：Flash/PSRAM 专用总线，复用会导致无法启动/崩溃；
    //   warn（黄）：Strapping / UART0 / USB，影响启动或烧录调试。
    // S31 / P4 / C6 的内置 Flash 具体 GPIO 号见各自数据手册，未在此硬编码。
    const FLASH_CHIPS = {
        6:  ['ESP32', 'ESP32-H4'],
        7:  ['ESP32', 'ESP32-H4'],
        8:  ['ESP32', 'ESP32-H4'],
        9:  ['ESP32', 'ESP32-H4'],
        10: ['ESP32', 'ESP32-H4'],
        11: ['ESP32', 'ESP32-H4', 'ESP32-C2', 'ESP32-C3'],
        12: ['ESP32-H4', 'ESP32-C2', 'ESP32-C3'],
        13: ['ESP32-C2', 'ESP32-C3'],
        14: ['ESP32-C2', 'ESP32-C3', 'ESP32-C61'],
        15: ['ESP32-C2', 'ESP32-C3', 'ESP32-C61', 'ESP32-H2'],
        16: ['ESP32', 'ESP32-C2', 'ESP32-C3', 'ESP32-C5', 'ESP32-C61', 'ESP32-H2'],
        17: ['ESP32', 'ESP32-C2', 'ESP32-C3', 'ESP32-C5', 'ESP32-C61', 'ESP32-H2'],
        18: ['ESP32-C5', 'ESP32-H2'],
        19: ['ESP32-C5', 'ESP32-C61', 'ESP32-H2'],
        20: ['ESP32-C5', 'ESP32-C61', 'ESP32-H2'],
        21: ['ESP32-C5', 'ESP32-C61', 'ESP32-H2'],
        22: ['ESP32-C5'],
        24: ['ESP32-C6'],
        25: ['ESP32-C6'],
        26: ['ESP32-S2', 'ESP32-S3', 'ESP32-C6'],
        27: ['ESP32-S2', 'ESP32-S3', 'ESP32-C6'],
        28: ['ESP32-S2', 'ESP32-S3', 'ESP32-C6'],
        29: ['ESP32-S2', 'ESP32-S3', 'ESP32-C6'],
        30: ['ESP32-S2', 'ESP32-S3', 'ESP32-C6'],
        31: ['ESP32-S2', 'ESP32-S3'],
        32: ['ESP32-S2', 'ESP32-S3'],
        33: ['ESP32-S3'],
        34: ['ESP32-S3'],
        35: ['ESP32-S3'],
        36: ['ESP32-S3'],
        37: ['ESP32-S3']
    };

    const STRAP_CHIPS = {
        0:  ['ESP32', 'ESP32-S2', 'ESP32-S3'],
        2:  ['ESP32', 'ESP32-C3', 'ESP32-C5', 'ESP32-H2'],
        3:  ['ESP32-S3', 'ESP32-H2'],
        4:  ['ESP32-C6'],
        5:  ['ESP32', 'ESP32-C6'],
        7:  ['ESP32-C5', 'ESP32-C61'],
        8:  ['ESP32-C2', 'ESP32-C3', 'ESP32-C6', 'ESP32-C61', 'ESP32-H2'],
        9:  ['ESP32-C2', 'ESP32-C3', 'ESP32-C6', 'ESP32-C61', 'ESP32-H2'],
        12: ['ESP32'],
        13: ['ESP32-H4'],
        14: ['ESP32-H4'],
        15: ['ESP32', 'ESP32-C6'],
        17: ['ESP32-H4'],
        25: ['ESP32-H2'],
        27: ['ESP32-C5'],
        28: ['ESP32-C5'],
        34: ['ESP32-P4'],
        35: ['ESP32-P4'],
        36: ['ESP32-P4', 'ESP32-H4'],
        37: ['ESP32-P4', 'ESP32-H4', 'ESP32-S31'],
        38: ['ESP32-P4'],
        45: ['ESP32-S2', 'ESP32-S3'],
        46: ['ESP32-S2', 'ESP32-S3'],
        60: ['ESP32-S31'],
        61: ['ESP32-S31']
    };

    const UART_CHIPS = {
        1:  ['ESP32'],
        3:  ['ESP32'],
        10: ['ESP32-C61'],
        11: ['ESP32-C5', 'ESP32-C61'],
        12: ['ESP32-C5'],
        16: ['ESP32-C6'],
        17: ['ESP32-C6'],
        19: ['ESP32-C2'],
        20: ['ESP32-C2', 'ESP32-C3'],
        21: ['ESP32-C3'],
        23: ['ESP32-H2'],
        24: ['ESP32-H2'],
        37: ['ESP32-P4'],
        38: ['ESP32-P4'],
        43: ['ESP32-S2', 'ESP32-S3'],
        44: ['ESP32-S2', 'ESP32-S3'],
        58: ['ESP32-S31'],
        59: ['ESP32-S31']
    };

    const USB_CHIPS = {
        12: ['ESP32-C6', 'ESP32-C61'],
        13: ['ESP32-C5', 'ESP32-C6', 'ESP32-C61', 'ESP32-H4'],
        14: ['ESP32-C5', 'ESP32-H4'],
        18: ['ESP32-C3'],
        19: ['ESP32-S2', 'ESP32-S3', 'ESP32-C3'],
        20: ['ESP32-S2', 'ESP32-S3'],
        24: ['ESP32-P4'],
        25: ['ESP32-P4'],
        26: ['ESP32-H2'],
        27: ['ESP32-H2'],
        33: ['ESP32-S31'],
        34: ['ESP32-S31']
    };

    function updatePinWarning(p, elId){
        const el = $(elId || 'pinWarning');
        if (!el) return;
        const groups = [
            { map: FLASH_CHIPS, level: 'danger', what: 'Flash/PSRAM 专用总线（SPI0/1）', impact: '复用会导致无法启动或运行崩溃' },
            { map: STRAP_CHIPS, level: 'warn',  what: 'Strapping 引脚',                impact: '复位时会决定启动/下载模式' },
            { map: UART_CHIPS,  level: 'warn',  what: 'UART0 引脚',                   impact: '占用后会失去串口烧录与日志输出' },
            { map: USB_CHIPS,   level: 'warn',  what: 'USB 引脚（USB-JTAG / USB-OTG）', impact: '占用后USB 自动下载失效（需手动拉低 BOOT 才能下载）' }
        ];
        const lines = [];
        groups.forEach(g => {
            const chips = g.map[p];
            if (chips && chips.length){
                lines.push({ level: g.level, chips: chips.join('、'), what: g.what, impact: g.impact });
            }
        });
        if (!lines.length){
            el.textContent = '';
            el.style.display = 'none';
            return;
        }
        el.innerHTML = lines.map(l =>
            '<div class="risk-' + l.level + '">该引脚在 <span class="chip-name">' + l.chips + '</span> 上属于' + l.what + '，' + l.impact + '。</div>'
        ).join('');
        el.style.display = 'block';
    }

    // ===== 引脚选择：锁存状态提示（PWM / 序列器 / 计数器 / 比较器 共用） =====
    // 锁存是引脚级"自持"：把锁存脚指派给任何模块都不会抢走它的电平，但该模块的输出会**不正确**
    // （各模块的 update* 都跳过锁存脚）。所以不拦、不置灰，只在选中引脚时 toast 提示。
    // 数据取自 /iostatus 的 latch 字段。
    let latchedPins = [];
    function refreshLatchedPins(){
        fetchJson('/iostatus').then(d => {
            latchedPins = (d.pins || []).filter(g => g.latch).map(g => g.pin);
        }).catch(() => {});
    }

    function openPinModal(i){
        editingChannel = i;
        pendingPins = CONFIG.pwmPins.slice();
        pwmDevMode = false;
        pwmAllowDup = false;
        const dsw = $('pwmDevModeSwitch'); if (dsw) dsw.checked = false;
        const asw = $('pwmAllowDupSwitch'); if (asw) asw.checked = false;
        $('pinModalTitle').textContent = '设置通道 ' + (i + 1) + ' 的 GPIO';
        renderPinGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时用 toast 提示）
        $('pinModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function closePinModal(){
        $('pinModal').classList.remove('show');
        editingChannel = -1;
        pendingPins = null;
        document.body.classList.remove('modal-open');
    }

    function renderPinGrid(){
        const current = editingChannel >= 0 ? pendingPins[editingChannel] : -1;
        updatePinWarning(current);
        const grid = $('pinGrid');
        // 收集被 IO / 比较器 / 计数器占用的引脚（改绑会导致写者冲突，后端会拒绝）
        const occupied = [];
        (IO.groups || []).forEach(g => occupied.push(g.pin));
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => occupied.push(a.pin));
            (g.inputs || []).forEach(i => { if (i.source === 2) occupied.push(i.refIndex); });
        });
        (COUNTER.tasks || []).forEach(t => (t.actions || []).forEach(a => occupied.push(a.pin)));
        grid.innerHTML = (CONFIG.availablePins || []).map(p => {
            const usedElsewhere = pendingPins.some((v, idx) => idx !== editingChannel && v === p);
            const isFlash = ioIsFlashPin(p);
            const isOccupied = occupied.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : ((usedElsewhere || isOccupied) ? 'pin-used' : ''));
            return '<button type="button" class="pin-item ' + cls + '" onclick="selectPin(' + p + ')">GPIO ' + p + '</button>';
        }).join('');
        pwmUpdateSaveBtn();
    }

    function selectPin(p){
        if (editingChannel < 0) return;
        // 不拦（锁存自持，指派不会改变它的电平），但当场说清后果：锁存优先，该通道的输出不正确
        if (latchedPins.indexOf(p) >= 0) toast('注意：GPIO' + p + ' 处于调度面板锁存中，该通道输出不正确');
        pendingPins[editingChannel] = p;
        transitionModalHeight($('pinModal'), () => renderPinGrid());
    }

    function savePins(){
        if (editingChannel < 0) return;
        fetchJson('/savepins', { method: 'POST', body: JSON.stringify({ pins: pendingPins, devMode: pwmDevMode, allowDup: pwmAllowDup }) })
            .then(d => {
                if (d.success){
                    const oldPins = CONFIG.pwmPins.slice();
                    CONFIG.pwmPins = pendingPins.slice();
                    renderAll();
                    closePinModal();
                    // 保存成功再提示一次：新增/改绑到的引脚里若有锁存脚，说明其输出不正确
                    const locked = pendingPins.filter((v, idx) => v !== oldPins[idx] && latchedPins.indexOf(v) >= 0);
                    toast(locked.length ? ('GPIO 引脚已保存；注意：GPIO' + locked.join('、') + ' 处于调度面板锁存中，输出不正确')
                                        : 'GPIO 引脚已保存');
                } else {
                    toast(d.error || '保存失败');
                }
            });
    }

    // 保存按钮禁用逻辑：与后端校验一致（Flash 需开发模式；占用/重复需允许重复选中）
    function pwmUpdateSaveBtn(){
        const btn = $('pwmSaveBtn');
        if (!btn) return;
        if (editingChannel < 0){ btn.disabled = true; return; }
        const p = pendingPins[editingChannel];
        if (p == null || p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const usedElsewhere = pendingPins.some((v, idx) => idx !== editingChannel && v === p);
        const occupied = [];
        (IO.groups || []).forEach(g => occupied.push(g.pin));
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => occupied.push(a.pin));
            (g.inputs || []).forEach(i => { if (i.source === 2) occupied.push(i.refIndex); });
        });
        (COUNTER.tasks || []).forEach(t => (t.actions || []).forEach(a => occupied.push(a.pin)));
        const isOccupied = occupied.indexOf(p) >= 0;
        btn.disabled = (isFlash && !pwmDevMode) || ((usedElsewhere || isOccupied) && !pwmAllowDup);
    }

    function pwmDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const asw = $('pwmAllowDupSwitch'); if (asw) asw.checked = false;
            $('pwmDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            pwmDevMode = false;
            pwmAllowDup = false;
            transitionModalHeight($('pinModal'), () => renderPinGrid());
        }
    }

    function pwmDevModeConfirm(){
        pwmDevMode = true;
        pwmAllowDup = $('pwmAllowDupSwitch') ? $('pwmAllowDupSwitch').checked : false;
        $('pwmDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('pwmDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('pinModal'), () => renderPinGrid());
    }

    function pwmDevModeCancel(){
        $('pwmDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('pwmDevModeSwitch'); if (sw) sw.checked = false;
    }

    // ===== PWM 通道增删 =====
    let pwmAddPendingPin = -1;
    let pwmAddDevMode = false;      // 开发模式：放开 Flash 禁用
    let pwmAddAllowDup = false;     // 允许重复选中引脚
    let pwmDelTarget = -1;

    function pwmAddOpen(){
        if ((CONFIG.pwmPins || []).length >= 8){ toast('通道数已达上限 8 个'); return; }
        pwmAddPendingPin = -1;
        pwmAddDevMode = false;
        pwmAddAllowDup = false;
        const dsw = $('pwmAddDevModeSwitch'); if (dsw) dsw.checked = false;
        const asw = $('pwmAddAllowDupSwitch'); if (asw) asw.checked = false;
        renderPwmAddGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时用 toast 提示）
        $('pwmAddModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function pwmAddClose(){
        $('pwmAddModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        pwmAddPendingPin = -1;
    }
    function renderPwmAddGrid(){
        const p = pwmAddPendingPin;
        updatePinWarning(p, 'pwmAddPinWarning');
        const used = (CONFIG.pwmPins || []).slice();
        // 收集被 IO / 比较器 / 计数器占用的引脚（与 PWM 输出互斥，除非开启「允许重复选中」）
        const occupied = [];
        (IO.groups || []).forEach(g => occupied.push(g.pin));
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => occupied.push(a.pin));
            (g.inputs || []).forEach(i => { if (i.source === 2) occupied.push(i.refIndex); });
        });
        (COUNTER.tasks || []).forEach(t => (t.actions || []).forEach(a => occupied.push(a.pin)));
        const grid = $('pwmAddPinGrid');
        grid.innerHTML = (CONFIG.availablePins || []).map(pin => {
            const isUsed = used.indexOf(pin) >= 0;
            const isFlash = ioIsFlashPin(pin);
            const isOccupied = occupied.indexOf(pin) >= 0;
            const cls = (pin === p) ? 'current' : (isFlash ? 'pin-danger' : ((isUsed || isOccupied) ? 'pin-used' : ''));
            return '<button type="button" class="pin-item ' + cls + '" onclick="pwmAddSelect(' + pin + ')">GPIO ' + pin + '</button>';
        }).join('');
        pwmAddUpdateConfirmBtn();
    }
    function pwmAddSelect(pin){
        pwmAddPendingPin = pin;
        if (latchedPins.indexOf(pin) >= 0) toast('注意：GPIO' + pin + ' 处于调度面板锁存中，该通道输出不正确');
        transitionModalHeight($('pwmAddModal'), () => renderPwmAddGrid());
    }
    function pwmAddConfirm(){
        if (pwmAddPendingPin < 0){ toast('请选择一个引脚'); return; }
        fetchJson('/pwmadd', { method: 'POST', body: JSON.stringify({ pin: pwmAddPendingPin, devMode: pwmAddDevMode, allowDup: pwmAddAllowDup }) })
            .then(d => {
                if (d.success){
                    const locked = latchedPins.indexOf(pwmAddPendingPin) >= 0;   // 关闭前先判断
                    toast(locked ? ('通道已添加；注意：GPIO' + pwmAddPendingPin + ' 处于调度面板锁存中，该通道输出不正确')
                                 : '通道已添加');
                    pwmAddClose();
                    loadConfig();
                }
                else toast(d.error || '添加失败');
            });
    }

    // 添加通道确认按钮禁用逻辑：与后端校验一致（Flash 需开发模式；占用/重复需允许重复选中）
    function pwmAddUpdateConfirmBtn(){
        const btn = $('pwmAddSaveBtn');
        if (!btn) return;
        const p = pwmAddPendingPin;
        if (p == null || p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const usedElsewhere = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        const occupied = [];
        (IO.groups || []).forEach(g => occupied.push(g.pin));
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => occupied.push(a.pin));
            (g.inputs || []).forEach(i => { if (i.source === 2) occupied.push(i.refIndex); });
        });
        (COUNTER.tasks || []).forEach(t => (t.actions || []).forEach(a => occupied.push(a.pin)));
        const isOccupied = occupied.indexOf(p) >= 0;
        btn.disabled = (isFlash && !pwmAddDevMode) || ((usedElsewhere || isOccupied) && !pwmAddAllowDup);
    }

    function pwmAddDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const asw = $('pwmAddAllowDupSwitch'); if (asw) asw.checked = false;
            $('pwmAddDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            pwmAddDevMode = false;
            pwmAddAllowDup = false;
            transitionModalHeight($('pwmAddModal'), () => renderPwmAddGrid());
        }
    }

    function pwmAddDevModeConfirm(){
        pwmAddDevMode = true;
        pwmAddAllowDup = $('pwmAddAllowDupSwitch') ? $('pwmAddAllowDupSwitch').checked : false;
        $('pwmAddDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('pwmAddDevModeSwitch'); if (sw) sw.checked = true;
        renderPwmAddGrid();
    }

    function pwmAddDevModeCancel(){
        $('pwmAddDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('pwmAddDevModeSwitch'); if (sw) sw.checked = false;
    }
    function pwmDel(i){
        pwmDelTarget = i;
        $('pwmDelDesc').textContent = '确认删除通道 ' + (i + 1) + ' 吗？删除后该 GPIO 将释放给 IO/比较器/计数器使用。';
        $('pwmDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function pwmDelCancel(){
        $('pwmDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        pwmDelTarget = -1;
    }
    function pwmDelConfirm(){
        if (pwmDelTarget < 0) return;
        const i = pwmDelTarget;
        pwmDelTarget = -1;
        $('pwmDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        fetchJson('/pwmdel', { method: 'POST', body: JSON.stringify({ index: i }) })
            .then(d => {
                if (d.success){ toast('通道已删除'); loadConfig(); }
                else toast(d.error || '删除失败');
            });
    }

    // ===== PWM 条目增删 =====
    function pwmTaskAdd(i){
        fetchJson('/pwmtaskadd', { method: 'POST', body: JSON.stringify({ channel: i }) })
            .then(d => {
                if (d.success){ toast('条目已添加'); loadConfig(); }
                else toast(d.error || '添加失败');
            })
            .catch(() => {});
    }

    let pwmTaskDelTarget = { ch: -1, idx: -1 };
    function pwmTaskDel(ch, idx){
        pwmTaskDelTarget = { ch: ch, idx: idx };
        $('pwmTaskDelDesc').textContent = '确认删除通道 ' + (ch + 1) + ' 的「条目' + (idx + 1) + '」吗？删除后不可恢复。';
        $('pwmTaskDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function pwmTaskDelCancel(){
        $('pwmTaskDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        pwmTaskDelTarget = { ch: -1, idx: -1 };
    }
    function pwmTaskDelConfirm(){
        const t = pwmTaskDelTarget;
        pwmTaskDelTarget = { ch: -1, idx: -1 };
        $('pwmTaskDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        if (t.ch < 0 || t.idx < 0) return;
        fetchJson('/pwmtaskdel', { method: 'POST', body: JSON.stringify({ channel: t.ch, index: t.idx }) })
            .then(d => {
                if (d.success){ toast('条目已删除'); loadConfig(); }
                else toast(d.error || '删除失败');
            })
            .catch(() => {});
    }

    // ===== WiFi =====
    function openWifi(){
        $('wifiModal').classList.add('show');
        document.body.classList.add('modal-open');
        scanWifi();
    }
    function closeWifi(){
        $('wifiModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function refreshWifiInfo(){ loadStatus(); }

    // 仿真扫描进度条：WiFi 扫描无真实进度接口，用「先快后慢」动画模拟，真实完成后跳到 100%
    let scanProgress = 0;
    let scanProgressTimer = null;

    function scanWifi(){
        const list = $('wifiList');
        const btn = $('scanBtn');
        btn.disabled = true;
        list.innerHTML =
            '<div class="wifi-scan-progress">' +
                '<div class="wifi-scan-track"><div class="wifi-scan-bar" id="wifiScanBar"></div></div>' +
                '<div class="wifi-scan-text" id="wifiScanText">正在扫描网络…</div>' +
            '</div>';
        startScanProgress();
        const poll = () => {
            fetchJson('/wifiscan').then(d => {
                if (d.scanning){ setTimeout(poll, 800); return; }
                finishScanProgress();
                btn.disabled = false;
                const nets = d.networks || [];
                if (!nets.length){ list.innerHTML = '<div style="font-size:13px;color:var(--text-dim);padding:8px 4px;">未发现网络</div>'; return; }
                nets.sort((a, b) => b.rssi - a.rssi);
                list.innerHTML = nets.map(n =>
                    '<div class="wifi-item" onclick="selectWifi(this)">' +
                        '<span class="ssid">' + esc(n.ssid) + '</span>' +
                        '<span class="meta">' + (n.open ? '开放' : '加密') + ' · ' + n.rssi + 'dBm</span>' +
                    '</div>'
                ).join('');
            });
        };
        poll();
    }

    function startScanProgress(){
        scanProgress = 0;
        if (scanProgressTimer) clearInterval(scanProgressTimer);
        updateScanBar();
        scanProgressTimer = setInterval(() => {
            if (scanProgress < 60){
                scanProgress += Math.random() * 3 + 1;       // 中速段
            } else if (scanProgress < 85){
                scanProgress += Math.random() * 9 + 4;       // 快速推进段
            } else if (scanProgress < 94){
                scanProgress += Math.random() * 0.7 + 0.15;  // 慢速爬升，停在 94% 等真实完成
            }
            if (scanProgress > 94) scanProgress = 94;
            updateScanBar();
        }, 120);
    }

    function updateScanBar(){
        const bar = $('wifiScanBar');
        if (bar) bar.style.width = scanProgress.toFixed(1) + '%';
    }

    function finishScanProgress(){
        if (scanProgressTimer){ clearInterval(scanProgressTimer); scanProgressTimer = null; }
        const bar = $('wifiScanBar');
        const txt = $('wifiScanText');
        if (bar) bar.style.width = '100%';
        if (txt) txt.textContent = '扫描完成';
    }

    let selectedSsid = '';
    function selectWifi(el){
        const items = el.parentElement.children;
        for (const it of items) it.classList.remove('selected');
        el.classList.add('selected');
        selectedSsid = el.querySelector('.ssid').textContent;
    }

    function saveWifi(){
        if (!selectedSsid){ toast('请先选择网络'); return; }
        const pwd = $('wifiPassword').value;
        fetchJson('/wifisave', { method: 'POST', body: JSON.stringify({ ssid: selectedSsid, password: pwd }) })
            .then(d => { if (d.success) toast('已保存，正在重启连接…'); else toast(d.error || '保存失败'); });
    }

    // ===== 串口日志 =====
    let logSeq = 0;              // 已拉取到的日志游标
    let logPolling = false;      // 是否正在轮询（弹窗打开时）
    let logAutoScroll = true;    // 是否自动滚动到底部（用户上翻后暂停）

    function openLogs(){
        $('logModal').classList.add('show');
        document.body.classList.add('modal-open');
        if (!logPolling){
            logPolling = true;
            pollLogs();
        }
    }
    function closeLogs(){
        $('logModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        logPolling = false;
    }

    // ===== 恢复出厂 / 清除配置 =====
    function confirmFactoryReset(){
        $('factoryResetModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeFactoryResetConfirm(){
        $('factoryResetModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function doFactoryReset(){
        closeFactoryResetConfirm();
        runReset('/factoryreset', '正在恢复出厂…', '设备将重启并恢复出厂状态',
            '设备已恢复出厂并重启。请重新连接热点 Espogic（密码 12345678），连接成功后点击下方按钮刷新。',
            false);
    }
    function confirmResetConfig(){
        $('resetConfigModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeResetConfigConfirm(){
        $('resetConfigModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function doResetConfig(){
        closeResetConfigConfirm();
        runReset('/resetconfig', '正在清除配置…', '设备将重启并加载默认配置',
            '配置已清除并重启，设备正在重新连接 WiFi，页面将自动刷新…',
            true);
    }

    // 重置通用流程：伪进度条 -> 等待设备重启 -> 手动引导或自动刷新
    function runReset(url, title, desc, guide, autoReload){
        $('resetProgressTitle').textContent = title;
        $('resetProgressDesc').textContent = desc;
        $('resetProgressBar').style.width = '0%';
        $('resetProgressRefreshBtn').style.display = 'none';
        $('resetProgressModal').classList.add('show');
        document.body.classList.add('modal-open');

        let p = 0;
        const timer = setInterval(() => {
            p += 6 + Math.random() * 12;
            if (p >= 90){ p = 90; clearInterval(timer); }
            $('resetProgressBar').style.width = p + '%';
        }, 120);

        fetchJson(url, { method: 'POST', body: JSON.stringify({}) })
            .then(d => {
                if (d.success){ clearInterval(timer); finishReset(guide, autoReload); }
                else {
                    clearInterval(timer);
                    $('resetProgressModal').classList.remove('show');
                    document.body.classList.remove('modal-open');
                    toast(d.error || '重置失败');
                }
            })
            .catch(() => { clearInterval(timer); finishReset(guide, autoReload); });
    }

    function finishReset(guide, autoReload){
        $('resetProgressBar').style.width = '100%';
        $('resetProgressDesc').textContent = guide;
        if (autoReload){
            startAutoReload();
        } else {
            $('resetProgressRefreshBtn').style.display = 'inline-block';
        }
    }

    // 自动刷新：定时探测设备恢复，成功后刷新页面（用于保留 WiFi 的重置，IP 不变）
    function startAutoReload(){
        let tries = 0;
        const poll = setInterval(() => {
            tries++;
            fetch(window.location.href, { cache: 'no-store' })
                .then(() => { clearInterval(poll); location.reload(); })
                .catch(() => { if (tries >= 60){ clearInterval(poll); } });
        }, 1000);
    }

    function pollLogs(){
        if (!logPolling) return;
        fetch('/getlogs?seq=' + logSeq)
            .then(r => r.text())
            .then(txt => {
                if (!logPolling) return;
                const box = $('logBox');
                const lines = txt.split('\n');
                for (const line of lines){
                    if (!line) continue;
                    const bar = line.indexOf('|');
                    if (bar < 0) continue;
                    const seq = parseInt(line.slice(0, bar), 10);
                    const content = line.slice(bar + 1);
                    if (isNaN(seq)) continue;
                    if (seq <= logSeq) continue;
                    logSeq = seq;
                    box.textContent += content + '\n';
                }
                // 仅在用户未上翻（处于底部）时自动滚动跟随
                if (logAutoScroll) box.scrollTop = box.scrollHeight;
            })
            .catch(() => {})
            .finally(() => { setTimeout(pollLogs, 500); });
    }

    // 用户手动滚动时：若上翻则暂停自动跟随，滚回底部则恢复
    (function(){
        document.addEventListener('DOMContentLoaded', () => {
            const box = $('logBox');
            box.addEventListener('scroll', () => {
                logAutoScroll = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
            });
        });
    })();

    // ===== 芯片信息与版本固定 =====
    let chipData = null;
    let chipSelectedModel = null;
    let chipResTimer = null;

    function openChipPanel(){
        $('chipModal').classList.add('show');
        document.body.classList.add('modal-open');
        loadChipInfo();
        // 资源占用是活数：弹窗开着时每 2 秒刷一次，关窗即停。
        // 必须走独立的 loadChipRes()，不能重跑 loadChipInfo() ——
        // 后者会清掉 chipSelectedModel 并重建型号列表，等于每 2 秒把用户的选择抹掉一次。
        if (chipResTimer) clearInterval(chipResTimer);
        chipResTimer = setInterval(loadChipRes, 2000);
    }
    function closeChipPanel(){
        if (chipResTimer){ clearInterval(chipResTimer); chipResTimer = null; }
        $('chipModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function chipFmtBytes(b){
        if (!b || b <= 0) return '—';
        if (b >= 1048576) return Math.round(b / 1048576) + 'MB';
        return Math.round(b / 1024) + 'KB';
    }
    // 与 chipFmtBytes 的区别：0 要显示成 0 而不是「—」（PSRAM 已用 0 是正常状态，不是缺数据）
    function chipFmtSize(b){
        if (b == null || b < 0) return '—';
        if (b >= 1048576) return (b / 1048576).toFixed(1) + 'MB';
        if (b >= 1024) return Math.round(b / 1024) + 'KB';
        return b + 'B';
    }
    // 只刷新「资源占用」那几行，不碰型号选择状态
    function renderChipRes(d){
        if (!d) return;
        if (d.heapTotal && d.heapFree != null){
            let t = chipFmtSize(d.heapTotal - d.heapFree) + ' / ' + chipFmtSize(d.heapTotal);
            if (d.heapMin != null) t += ' · 最低 ' + chipFmtSize(d.heapMin);   // 开机至今最低水位
            $('chipHeap').textContent = t;
        } else $('chipHeap').textContent = '—';

        if (d.psramTotal && d.psramFree != null){
            $('chipPsram').textContent = chipFmtSize(d.psramTotal - d.psramFree) + ' / ' + chipFmtSize(d.psramTotal);
        } else $('chipPsram').textContent = '无';

        if (d.nvsPercent != null){
            // 后端给的是"已用百分比"（分母已按 used+avail 修正过，见 handleChipInfo 注释）
            const pct = 100 - d.nvsPercent;
            // nvsAvail 的单位是条目，每条 32 字节
            const kb = (d.nvsAvail * 32 / 1024).toFixed(1);
            $('chipNvs').textContent = pct + '%（可用 ' + kb + 'KB）';
        } else $('chipNvs').textContent = '—';

        // 负载率在前（它是结论），迭代率跟在括号里（它是依据）
        if (d.loopLoad != null && d.loopRate != null){
            $('chipLoopRate').textContent = d.loopLoad + '%（' + d.loopRate + ' 次/秒）';
        } else $('chipLoopRate').textContent = (d.loopRate != null) ? (d.loopRate + ' 次/秒') : '—';
        if (d.loopMaxMs != null){
            // 迭代率骤降 + 卡顿变大 = CPU 正被 WiFi 驱动 / HTTP / 天气抓取占走
            $('chipLoopStall').textContent = '最大 ' + d.loopMaxMs + 'ms · ' + (d.loopStalls || 0) + ' 次';
        } else $('chipLoopStall').textContent = '—';
    }
    // 轮询用裸 fetch：fetchJson 失败会弹 toast，2 秒一次会把界面刷爆
    function loadChipRes(){
        fetch('/chipinfo').then(r => r.json()).then(renderChipRes).catch(() => {});
    }
    function loadChipInfo(){
        fetchJson('/chipinfo').then(d => {
            chipData = d;
            chipSelectedModel = null;
            $('chipRealModel').textContent = d.realModel || '—';
            $('chipEffModel').textContent = d.model || '—';
            $('chipRevision').textContent = (d.revision != null) ? ('Rev ' + d.revision) : '—';
            $('chipStorage').textContent = 'Flash ' + chipFmtBytes(d.flashSize) + ' · PSRAM ' + chipFmtBytes(d.psramSize);
            $('chipTemp').textContent = (d.temperature != null && !isNaN(d.temperature)) ? Math.round(d.temperature) + '°C' : '—';
            renderChipRes(d);
            $('chipForcedTag').textContent = d.forced ? '已固定（重启后保持）' : '自动识别';
            $('chipForcedTag').className = d.forced ? 'chip-forced-on' : 'chip-forced-off';
            renderChipModelList();
        }).catch(() => {});
    }
    function renderChipModelList(){
        if (!chipData) return;
        const eff = chipData.model;
        $('chipModelList').innerHTML = (chipData.models || []).map(m => {
            const isEff = (m === eff);
            const isSel = (m === chipSelectedModel);
            const cls = 'chip-model-item' + (isSel ? ' selected' : '');
            return '<button type="button" class="' + cls + '" onclick="chipSelect(\'' + m + '\')">' +
                '<span class="chip-model-name">' + esc(m) + '</span>' +
                (isEff ? '<span class="chip-model-tag">当前</span>' : '') +
            '</button>';
        }).join('');
        const btn = $('chipFixBtn');
        if (btn) btn.disabled = (chipSelectedModel == null);
    }
    function chipSelect(m){
        chipSelectedModel = m;
        renderChipModelList();
    }
    function chipFix(){
        if (!chipSelectedModel){ toast('请先选择一个芯片型号'); return; }
        fetchJson('/chipset', { method: 'POST', body: JSON.stringify({ model: chipSelectedModel }) })
            .then(d => {
                if (d.success){
                    toast('已固定到 ' + chipSelectedModel + '（已写入 NVS）');
                    loadChipInfo();
                    loadStatus();
                } else {
                    toast(d.error || '固定失败');
                }
            });
    }
    function chipRestoreAuto(){
        fetchJson('/chipclear', { method: 'POST', body: JSON.stringify({}) })
            .then(d => {
                if (d.success){
                    toast('已恢复自动识别');
                    loadChipInfo();
                    loadStatus();
                } else {
                    toast(d.error || '操作失败');
                }
            });
    }

    )HTML";

#endif
