// webpage_js_io.h - 从 webpage.h 拆分而来
#ifndef ESPOGIC_WEBPAGE_JS_IO_H
#define ESPOGIC_WEBPAGE_JS_IO_H

static const char PAGE_JS_IO[] PROGMEM = R"HTML(// ===== 序列器 =====
    let IO = { groups: [] };
    let ioEditingGroup = -1;
    let ioEditingEntry = -1;
    let ioPendingPin = -1;
    let ioPendingAction = true;
    let ioPendingMode = 0;              // 0=定时开关 1=瞬时脉冲 2=持久心跳
    let ioAllowHbMode = true;           // 当前序列是否允许设为持久心跳（持久心跳独占序列）
    let ioAllowTimeMode = true;         // 当前序列是否允许设为定时开关/瞬时脉冲
    let ioDevMode = false;
    let ioAllowPwm = false;
    let ioAllowDup = false;
    let ioDelTarget = null;

    function loadIoConfig(){
        return fetchJson('/iogetconfig').then(cfg => { IO = cfg; ioRender(); }).catch(() => {});
    }

    function switchSection(s){
        $('tabPwm').classList.toggle('active', s === 'pwm');
        $('tabIo').classList.toggle('active', s === 'io');
        $('tabComparator').classList.toggle('active', s === 'comparator');
        $('tabCounter').classList.toggle('active', s === 'counter');
        $('pwmSection').style.display = (s === 'pwm') ? '' : 'none';
        $('ioSection').style.display = (s === 'io') ? '' : 'none';
        $('comparatorSection').style.display = (s === 'comparator') ? '' : 'none';
        $('counterSection').style.display = (s === 'counter') ? '' : 'none';
    }

    function ioRender(){
        let html = '';
        (IO.groups || []).forEach((g, gi) => {
            let entriesHtml = '';
            // 持久心跳在序列内独占：已有时不能再往该序列添加任何条目
            const hasHb = (g.entries || []).some(e => (e.mode || 0) === 2);
            (g.entries || []).forEach((e, ei) => {
                const conflictCls = e.conflict ? ' io-entry-conflict' : '';
                const noPermCls = e.noPerm ? ' io-entry-noperm' : '';
                const mode = e.mode || 0;
                const modeCls = (mode === 1) ? ' io-entry-pulse' : ((mode === 2) ? ' io-entry-hb' : '');
                let levelHtml = '';
                let modeHtml = '';
                if (mode === 1){
                    modeHtml = '<span class="io-mode-tag">脉冲 ' + e.pulseMs + 'ms</span>';
                    levelHtml = '<span class="io-action ' + (e.action ? 'io-on' : 'io-off') + '">' + (e.action ? '拉高' : '拉低') + '</span>';
                } else if (mode === 2){
                    modeHtml = '<span class="io-mode-tag">持久心跳 高' + e.hbHighMs + '/低' + e.hbLowMs + 'ms</span>';
                } else {
                    levelHtml = '<span class="io-action ' + (e.action ? 'io-on' : 'io-off') + '">' + (e.action ? '开启' : '关闭') + '</span>';
                }
                // 心跳没有时间维度，时间位显示「永久」而不是 00:00
                const timeHtml = (mode === 2)
                    ? '<span class="io-time io-time-forever">永久</span>'
                    : '<span class="io-time">' + pad2(e.hour) + ':' + pad2(e.minute) + '</span>';
                entriesHtml += '' +
                    '<div class="io-entry' + conflictCls + noPermCls + modeCls + '" onclick="ioEntryEditOpen(' + gi + ',' + ei + ')">' +
                        timeHtml +
                        levelHtml +
                        modeHtml +
                        (e.conflict ? '<span class="io-conflict-tag">冲突</span>' : '') +
                        (e.noPerm ? '<span class="io-noperm-tag">无权限</span>' : '') +
                        '<button class="icon-btn io-del" onclick="event.stopPropagation();ioEntryDel(' + gi + ',' + ei + ')">✕</button>' +
                    '</div>';
            });
            html += '' +
            '<section class="card channel' + (g.enabled ? ' on' : '') + '">' +
                '<div class="channel-head">' +
                    '<div class="channel-title-wrap">' +
                        '<span class="channel-dot"></span>' +
                        '<button type="button" class="pin-chip" onclick="ioGroupEditOpen(' + gi + ')">' + esc(g.name) + ' · GPIO ' + g.pin + ' <span class="edit-hint">编辑</span></button>' +
                    '</div>' +
                    '<div class="channel-actions">' +
                        '<button class="btn tonal" onclick="ioGroupDel(' + gi + ')">删除序列</button>' +
                        '<label class="switch-row" style="cursor:pointer;">' +
                            '<input type="checkbox" class="switch" ' + (g.enabled ? 'checked' : '') + ' onchange="ioToggle(' + gi + ', this)">' +
                        '</label>' +
                    '</div>' +
                '</div>' +
                '<div class="io-entries">' + (entriesHtml || '<div class="io-empty">暂无条目，点击下方按钮添加</div>') + '</div>' +
                '<div class="channel-footer" style="justify-content:flex-start;">' +
                    '<button class="btn tonal"' + (hasHb ? ' disabled' : '') + ' onclick="ioEntryAddOpen(' + gi + ')">＋ 添加条目</button>' +
                    (hasHb ? '<span class="io-hint">持久心跳独占该序列，不能再添加条目</span>' : '') +
                '</div>' +
            '</section>';
        });
        $('ioGroups').innerHTML = html || '<div class="card io-empty">暂无序列，点击上方「添加序列」创建</div>';
    }

    function ioToggle(gi, sw){
        fetchJson('/iotoggle', { method: 'POST', body: JSON.stringify({ index: gi }) }).then(d => {
            if (d.success){
                sw.checked = d.enabled;
                const card = sw.closest('.channel'); if (card) card.classList.toggle('on', d.enabled);
                toast(d.enabled ? '序列已开启' : '序列已关闭');
            } else {
                sw.checked = !sw.checked;
                toast(d.error || '操作失败');
            }
        });
    }

    function ioIsFlashPin(p){
        if (!CONFIG || !CONFIG.chipModel) return false;
        const chips = FLASH_CHIPS[p];
        if (!chips) return false;
        return chips.indexOf(CONFIG.chipModel) >= 0;   // 数组 indexOf = 精确匹配当前芯片
    }

    function ioRenderPinGrid(){
        const grid = $('ioPinGrid');
        if (!CONFIG || !CONFIG.availablePins){ grid.innerHTML = ''; return; }
        const usedByPwm = CONFIG.pwmPins || [];
        const usedByOther = (IO.groups || []).filter((g, gi) => gi !== ioEditingGroup).map(g => g.pin);
        const current = ioPendingPin;
        grid.innerHTML = CONFIG.availablePins.map(p => {
            const isFlash = ioIsFlashPin(p);
            const isUsed = usedByPwm.indexOf(p) >= 0 || usedByOther.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : (isUsed ? 'pin-used' : ''));
            return '<button type="button" class="pin-item ' + cls + '" onclick="ioSelectPin(' + p + ')">GPIO ' + p + '</button>';
        }).join('');
        updatePinWarning(current, 'ioPinWarning');
        ioUpdateSaveBtn();
    }

    function ioUpdateSaveBtn(){
        const btn = $('ioGroupSaveBtn');
        if (!btn) return;
        const p = ioPendingPin;
        if (p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const isPwm = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        const isOther = (IO.groups || []).some((g, gi) => gi !== ioEditingGroup && g.pin === p);
        btn.disabled = (isFlash && !ioDevMode) || (isPwm && !ioAllowPwm) || (isOther && !ioAllowDup);
    }

    function ioSelectPin(p){
        ioPendingPin = p;
        if (latchedPins.indexOf(p) >= 0) toast('注意：GPIO' + p + ' 处于调度面板锁存中，该序列输出不正确');
        transitionModalHeight($('ioGroupModal'), () => ioRenderPinGrid());
    }

    function ioGroupAddOpen(){
        ioEditingGroup = -1;
        ioPendingPin = -1;
        ioDevMode = false;
        ioAllowPwm = false;
        ioAllowDup = false;
        const sw = $('ioDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('ioAllowPwmSwitch'); if (ps) ps.checked = false;
        const ds = $('ioAllowDupSwitch'); if (ds) ds.checked = false;
        $('ioGroupTitle').textContent = '添加序列';
        $('ioGroupName').value = '';
        $('ioGroupSaveBtn').textContent = '添加';
        ioRenderPinGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('ioGroupModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioGroupEditOpen(gi){
        const g = IO.groups[gi];
        ioEditingGroup = gi;
        ioPendingPin = g.pin;
        ioDevMode = false;
        ioAllowPwm = false;
        ioAllowDup = false;
        const sw = $('ioDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('ioAllowPwmSwitch'); if (ps) ps.checked = false;
        const ds = $('ioAllowDupSwitch'); if (ds) ds.checked = false;
        $('ioGroupTitle').textContent = '编辑序列';
        $('ioGroupName').value = g.name;
        $('ioGroupSaveBtn').textContent = '保存';
        ioRenderPinGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('ioGroupModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioGroupClose(){
        $('ioGroupModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        ioEditingGroup = -1;
    }

    function ioDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const ps = $('ioAllowPwmSwitch'); if (ps) ps.checked = false;
            const ds = $('ioAllowDupSwitch'); if (ds) ds.checked = false;
            $('ioDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            ioDevMode = false;
            ioAllowPwm = false;
            ioAllowDup = false;
            transitionModalHeight($('ioGroupModal'), () => ioRenderPinGrid());
        }
    }

    function ioDevModeCancel(){
        $('ioDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('ioDevModeSwitch'); if (sw) sw.checked = false;
    }

    function ioDevModeConfirm(){
        ioDevMode = true;
        ioAllowPwm = $('ioAllowPwmSwitch') ? $('ioAllowPwmSwitch').checked : false;
        ioAllowDup = $('ioAllowDupSwitch') ? $('ioAllowDupSwitch').checked : false;
        $('ioDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('ioDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('ioGroupModal'), () => ioRenderPinGrid());
    }

    function ioGroupSave(){
        const name = $('ioGroupName').value.trim();
        if (ioPendingPin < 0){ toast('请选择一个 GPIO 引脚'); return; }
        const url = (ioEditingGroup < 0) ? '/iogroupadd' : '/iogroupupdate';
        const body = (ioEditingGroup < 0)
            ? { name: name, pin: ioPendingPin, devMode: ioDevMode, allowPwm: ioAllowPwm, allowDup: ioAllowDup }
            : { index: ioEditingGroup, name: name, pin: ioPendingPin, devMode: ioDevMode, allowPwm: ioAllowPwm, allowDup: ioAllowDup };
        fetchJson(url, { method: 'POST', body: JSON.stringify(body) }).then(d => {
            if (d.success){ toast('序列已保存'); ioGroupClose(); loadIoConfig(); }
            else toast(d.error || '保存失败');
        });
    }

    function ioGroupDel(gi){
        const g = IO.groups[gi];
        ioDelTarget = { type: 'group', gi: gi };
        $('ioDelDesc').textContent = '确认删除该序列下所有条目？' + (g ? '（序列名：' + g.name + '）' : '');
        $('ioDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioEntryAddOpen(gi){
        const list = (IO.groups[gi] && IO.groups[gi].entries) || [];
        // 持久心跳在序列内独占：已有持久心跳时不能再往该序列添加任何条目
        if (list.some(e => (e.mode || 0) === 2)){
            toast('该序列已有持久心跳，不能再添加其他条目');
            return;
        }
        ioAllowTimeMode = true;
        ioAllowHbMode = (list.length === 0);   // 序列非空时不能再添加持久心跳
        ioEditingGroup = gi;
        ioEditingEntry = -1;
        ioPendingAction = true;
        ioPendingMode = 0;
        $('ioEntryTitle').textContent = '添加条目';
        $('ioEntryHour').value = '';
        $('ioEntryMinute').value = '';
        $('ioPulseMs').value = '';
        $('ioHbHighMs').value = '';
        $('ioHbLowMs').value = '';
        ioEntryUpdateModeUI();
        $('ioEntryModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioEntryEditOpen(gi, ei){
        const e = IO.groups[gi].entries[ei];
        const others = (IO.groups[gi].entries || []).filter((x, i) => i !== ei);
        // 持久心跳独占：本条之外没有其他条目才允许设为持久心跳；
        // 其他条目里没有持久心跳才允许改成定时开关/瞬时脉冲
        ioAllowHbMode = (others.length === 0);
        ioAllowTimeMode = !others.some(x => (x.mode || 0) === 2);
        ioEditingGroup = gi;
        ioEditingEntry = ei;
        ioPendingAction = !!e.action;
        ioPendingMode = e.mode || 0;
        // 兜底：读到"持久心跳 + 其他条目"的非法老配置时心跳已不可能保留，
        // 默认切到定时开关，避免用户点保存必然失败
        if (ioPendingMode === 2 && !ioAllowHbMode) ioPendingMode = 0;
        $('ioEntryTitle').textContent = '编辑条目';
        $('ioEntryHour').value = e.hour;
        $('ioEntryMinute').value = e.minute;
        $('ioPulseMs').value = e.pulseMs || '';
        $('ioHbHighMs').value = e.hbHighMs || '';
        $('ioHbLowMs').value = e.hbLowMs || '';
        ioEntryUpdateModeUI();
        $('ioEntryModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioEntryClose(){
        $('ioEntryModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        ioEditingEntry = -1;
    }

    function ioEntrySetAction(on){
        ioPendingAction = on;
        ioEntryUpdateActionBtn();
    }

    function ioEntrySetMode(m){
        ioPendingMode = m;
        // 切模式会同时换掉说明文字与显隐的行，统一走高度过渡避免弹窗跳变
        transitionModalHeight($('ioEntryModal'), () => ioEntryUpdateModeUI());
    }

    // 模式切换：持久心跳无时间维度也不选动作电平（固定先高后低）；脉冲下"开启/关闭"语义变为"拉高/拉低"
    function ioEntryUpdateModeUI(){
        // 说明文字只显示当前模式对应那一段
        $('ioEntryDesc').textContent = (ioPendingMode === 1)
            ? '瞬时脉冲：到点拉高/拉低指定毫秒后回到基础电平。'
            : ((ioPendingMode === 2)
                ? '持久心跳：不需要时间，序列启用期间永久按毫秒高低循环。'
                : '定时开关：到点输出并保持。');
        $('ioModeSchedule').className = (ioPendingMode === 0) ? 'btn filled' : 'btn tonal';
        $('ioModePulse').className    = (ioPendingMode === 1) ? 'btn filled' : 'btn tonal';
        $('ioModeHb').className       = (ioPendingMode === 2) ? 'btn filled' : 'btn tonal';
        // 持久心跳在序列内独占：不允许的模式直接禁用并说明原因（后端也会再挡一次）
        $('ioModeSchedule').disabled = !ioAllowTimeMode;
        $('ioModePulse').disabled    = !ioAllowTimeMode;
        $('ioModeHb').disabled       = !ioAllowHbMode;
        const modeHint = $('ioModeHint');
        if (!ioAllowHbMode){
            modeHint.textContent = '该序列已有其他条目：持久心跳必须独占序列，不能再添加。';
            modeHint.style.display = '';
        } else if (!ioAllowTimeMode){
            modeHint.textContent = '该序列已启用持久心跳：不能再添加定时开关 / 瞬时脉冲。';
            modeHint.style.display = '';
        } else {
            modeHint.style.display = 'none';
        }
        $('ioEntryTimeRow').style.display = (ioPendingMode === 2) ? 'none' : '';
        $('ioActRow').style.display   = (ioPendingMode === 2) ? 'none' : '';
        $('ioPulseRow').style.display = (ioPendingMode === 1) ? '' : 'none';
        $('ioHbRow').style.display    = (ioPendingMode === 2) ? '' : 'none';
        $('ioActOn').textContent  = (ioPendingMode === 1) ? '拉高' : '开启';
        $('ioActOff').textContent = (ioPendingMode === 1) ? '拉低' : '关闭';
        ioEntryUpdateActionBtn();
    }

    function ioEntryUpdateActionBtn(){
        $('ioActOn').className = ioPendingAction ? 'btn filled' : 'btn tonal';
        $('ioActOff').className = ioPendingAction ? 'btn tonal' : 'btn filled';
    }

    // 读取毫秒时长：val() 是静默夹紧、不报错，所以超范围必须显式提示（不要把夹紧当校验）。
    // 非法返回 -1，由调用方直接 return。
    function ioReadMs(id, label){
        const raw = $(id).value;
        if (raw === '' || isNaN(raw)){ toast('请填写' + label); return -1; }
        const n = Number(raw);
        if (n < 10 || n > 3600000){ toast(label + '需在 10 ~ 3600000 毫秒之间'); return -1; }
        return Math.round(n);
    }

    function ioEntrySave(){
        let h = val('ioEntryHour', 0, 0, 23), m = val('ioEntryMinute', 0, 0, 59);
        // 心跳无时间维度：清零后提交（后端 ioAddEntry/ioUpdateEntry 也会强制归零）
        if (ioPendingMode === 2){ h = 0; m = 0; }
        let pulseMs = 0, hbHighMs = 0, hbLowMs = 0;
        if (ioPendingMode === 1){
            pulseMs = ioReadMs('ioPulseMs', '脉冲时长');
            if (pulseMs < 0) return;
        } else if (ioPendingMode === 2){
            hbHighMs = ioReadMs('ioHbHighMs', '持久心跳高电平时长');
            if (hbHighMs < 0) return;
            hbLowMs = ioReadMs('ioHbLowMs', '持久心跳低电平时长');
            if (hbLowMs < 0) return;
        }
        const url = (ioEditingEntry < 0) ? '/ioentryadd' : '/ioentryupdate';
        const body = {
            groupIndex: ioEditingGroup, hour: h, minute: m,
            action: ioPendingAction ? 1 : 0, mode: ioPendingMode,
            pulseMs: pulseMs, hbHighMs: hbHighMs, hbLowMs: hbLowMs
        };
        if (ioEditingEntry >= 0) body.entryIndex = ioEditingEntry;
        fetchJson(url, { method: 'POST', body: JSON.stringify(body) }).then(d => {
            if (d.success){ toast('条目已保存'); ioEntryClose(); loadIoConfig(); }
            else toast(d.error || '保存失败');
        });
    }

    function ioEntryDel(gi, ei){
        ioDelTarget = { type: 'entry', gi: gi, ei: ei };
        $('ioDelDesc').textContent = '确认删除此条目？';
        $('ioDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    function ioDelCancel(){
        $('ioDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        ioDelTarget = null;
    }

    function ioDelConfirm(){
        if (!ioDelTarget) return;
        const t = ioDelTarget;
        ioDelTarget = null;
        $('ioDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        if (t.type === 'group'){
            fetchJson('/iogroupdel', { method: 'POST', body: JSON.stringify({ index: t.gi }) }).then(d => {
                if (d.success){ toast('序列已删除'); loadIoConfig(); }
                else toast(d.error || '删除失败');
            });
        } else {
            fetchJson('/ioentrydel', { method: 'POST', body: JSON.stringify({ groupIndex: t.gi, entryIndex: t.ei }) }).then(d => {
                if (d.success){ toast('条目已删除'); loadIoConfig(); }
                else toast(d.error || '删除失败');
            });
        }
    }

    // ===== 调度面板（管理所有可用 GPIO 引脚，手动覆盖优先） =====
    let ioPanelData = [];
    let ioPanelConfirmPin = -1;
    let ioPanelDevMode = false;   // 开发模式：放开 PSRAM 引脚限制
    let ioPanelAllowPwm = false;  // 允许切换 PWM 通道引脚

    function openIoPanel(){
        $('ioPanelModal').classList.add('show');
        document.body.classList.add('modal-open');
        refreshIoPanel();
    }
    function closeIoPanel(){
        $('ioPanelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function ioPanelDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const ps = $('ioPanelAllowPwmSwitch'); if (ps) ps.checked = false;
            $('ioPanelDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            ioPanelDevMode = false;
            ioPanelAllowPwm = false;
            renderIoPanel();
        }
    }
    function ioPanelDevModeCancel(){
        $('ioPanelDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('ioPanelDevModeSwitch'); if (sw) sw.checked = false;
    }
    function ioPanelDevModeConfirm(){
        ioPanelDevMode = true;
        ioPanelAllowPwm = $('ioPanelAllowPwmSwitch') ? $('ioPanelAllowPwmSwitch').checked : false;
        $('ioPanelDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('ioPanelDevModeSwitch'); if (sw) sw.checked = true;
        renderIoPanel();
    }
    function refreshIoPanel(){
        fetchJson('/iostatus').then(d => {
            ioPanelData = d.pins || [];
            renderIoPanel();
        }).catch(() => {});
    }
    function renderIoPanel(){
        const list = $('ioPanelList');
        const hasManual = ioPanelData.some(g => g.manual);
        const autoBtn = $('ioPanelAutoBtn');
        if (autoBtn) autoBtn.disabled = !hasManual;
        if (!ioPanelData.length){
            list.innerHTML = '<div class="io-empty" style="grid-column:1 / -1;">无可用引脚</div>';
            return;
        }
        list.innerHTML = ioPanelData.map(g => {
            const isPwm = !!g.pwm;
            const isFlash = !!g.flash;
            const isInput = !!g.input;
            // 只读输入引脚（比较器外部输入/计数器触发源）：任何模式下都不允许手动切换或锁存
            const disabled = (!g.latch) && (isInput || (isFlash && !ioPanelDevMode) || (isPwm && !ioPanelAllowPwm));
            let lvlHtml;
            if (g.level < 0){
                lvlHtml = '';   // PWM 正常输出引脚不显示高低电平
            } else {
                const lvlCls = g.level ? 'lvl-high' : 'lvl-low';
                const lvlText = g.level ? '高电平' : '低电平';
                lvlHtml = '<span class="' + lvlCls + '">' + lvlText + '</span>';
            }
            const pwmTag = isPwm ? '<span class="badge" style="background:rgba(79,209,197,.1);border-color:rgba(79,209,197,.32);color:#4fd1c5;">PWM</span>' : '';
            const flashTag = isFlash ? '<span class="badge" style="background:rgba(255,122,114,.12);border-color:rgba(255,122,114,.4);color:#ff7a72;">PSRAM</span>' : '';
            const inputTag = isInput ? '<span class="badge" style="background:rgba(122,162,247,.12);border-color:rgba(122,162,247,.35);color:#7aa2f7;">输入</span>' : '';
            const manualTag = g.manual ? '<span class="badge">手动</span>' : '';
            const latchTag = g.latch ? '<span class="badge" style="background:rgba(255,196,107,.1);border-color:rgba(255,196,107,.32);color:#ffc46b;">锁存' + (g.latchLevel ? '高' : '低') + '</span>' : '';
            const click = disabled ? '' : ' onclick="ioPanelClick(' + g.pin + ')"';
            const tip = isInput ? ' title="只读输入引脚（比较器外部输入/计数器触发源），不能手动切换或锁存"' : '';
            return '<div class="io-panel-item' + (disabled ? ' disabled' : '') + '"' + click + tip + '>' +
                '<span class="io-panel-name">GPIO ' + g.pin + '</span>' +
                lvlHtml +
                pwmTag + flashTag + inputTag + manualTag + latchTag +
            '</div>';
        }).join('');
    }
    function ioPanelClick(pin){
        const g = ioPanelData.find(x => x.pin === pin);
        if (!g) return;
        ioPanelConfirmPin = pin;
        const target = g.level ? '低电平' : '高电平';
        const cls = g.level ? 'lvl-low' : 'lvl-high';
        $('ioPanelConfirmDesc').innerHTML = '是否将「GPIO ' + pin + '」切换至<span class="' + cls + '">' + target + '</span>？';
        const w = $('ioPanelConfirmWarning');
        w.style.display = 'none';
        w.innerHTML = '';
        updatePinWarning(pin, 'ioPanelConfirmWarning');   // 显示 Flash/Strapping/UART/USB 风险提示
        if (g.pwm){
            w.style.display = 'block';
            w.innerHTML += '<div class="risk-warn">该引脚为 PWM 通道，切换后将暂停其 PWM 输出。</div>';
        }
        // 锁存状态：锁存时禁用切换，仅可解除
        const info = $('ioPanelLatchInfo');
        const doBtn = $('ioPanelConfirmDoBtn');
        const latchBtn = $('ioPanelLatchBtn');
        const autoBtn2 = $('ioPanelConfirmAutoBtn');
        if (g.latch){
            doBtn.disabled = true;
            info.style.display = 'block';
            info.innerHTML = '<div class="risk-warn">该引脚已锁存为 <b>' + (g.latchLevel ? '高电平' : '低电平') + '</b>，切换被禁用，仅可解除锁存。</div>';
            latchBtn.textContent = '解除锁存';
        } else {
            doBtn.disabled = false;
            info.style.display = 'none';
            info.innerHTML = '';
            latchBtn.textContent = '锁存';
        }
        // 「自动」：锁存时禁用；非手动基准时也禁用（本就处于自动，点了是空操作）
        if (autoBtn2) autoBtn2.disabled = g.latch || !g.manual;
        $('ioPanelConfirmModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function ioPanelConfirmCancel(){
        $('ioPanelConfirmModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        ioPanelConfirmPin = -1;
    }
    // 恢复该引脚的自动（清除手动基准）。锁存时禁用（锁存优先级最高，须先解除）；
    // 本来就不是手动基准时也禁用 —— 与面板上「恢复全部自动」的 autoBtn.disabled = !hasManual 一致。
    function ioPanelConfirmAuto(){
        if (ioPanelConfirmPin < 0) return;
        const pin = ioPanelConfirmPin;
        fetchJson('/ioautoreset', { method: 'POST', body: JSON.stringify({ pin: pin }) }).then(d => {
            ioPanelConfirmCancel();
            if (d.success){ toast('GPIO ' + pin + ' 已恢复自动'); refreshIoPanel(); }
            else toast(d.error || '操作失败');
        });
    }
    function ioPanelConfirmDo(){
        if (ioPanelConfirmPin < 0) return;
        const pin = ioPanelConfirmPin;
        const g = ioPanelData.find(x => x.pin === pin);
        if (!g) return;
        const newLevel = g.level ? 0 : 1;
        fetchJson('/iosetlevel', { method: 'POST', body: JSON.stringify({ pin: pin, level: newLevel, devMode: ioPanelDevMode, allowPwm: ioPanelAllowPwm }) }).then(d => {
            ioPanelConfirmCancel();
            if (d.success){ toast('已切换'); refreshIoPanel(); }
            else toast(d.error || '切换失败');
        });
    }
    function ioPanelAutoReset(){
        $('ioPanelAutoResetModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function ioPanelAutoResetCancel(){
        $('ioPanelAutoResetModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function ioPanelAutoResetConfirm(){
        $('ioPanelAutoResetModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        fetchJson('/ioautoreset', { method: 'POST', body: JSON.stringify({}) }).then(d => {
            if (d.success){ toast('已恢复自动'); refreshIoPanel(); }
            else toast(d.error || '操作失败');
        });
    }

    // ===== 锁存（最高权限） =====
    let latchPin = -1;
    let latchPendingLevel = 1;

    function ioPanelLatchAction(){
        const g = ioPanelData.find(x => x.pin === ioPanelConfirmPin);
        if (!g) return;
        if (g.latch){ latchClearDo(); }
        else { latchOpen(); }
    }
    function latchOpen(){
        if (ioPanelConfirmPin < 0) return;
        latchPin = ioPanelConfirmPin;
        latchPendingLevel = 1;
        latchUpdateLevelBtn();
        const g = ioPanelData.find(x => x.pin === latchPin);
        const isFlash = g ? !!g.flash : false;
        const isPwm = g ? !!g.pwm : false;
        $('latchSaveBtn').disabled = (isFlash && !ioPanelDevMode) || (isPwm && !ioPanelAllowPwm);
        updatePinWarning(latchPin, 'latchWarning');
        $('latchModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function latchClose(){
        $('latchModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        latchPin = -1;
    }
    function latchSetLevel(l){
        latchPendingLevel = l;
        latchUpdateLevelBtn();
    }
    function latchUpdateLevelBtn(){
        $('latchLvlHigh').className = latchPendingLevel ? 'btn filled' : 'btn tonal';
        $('latchLvlLow').className = latchPendingLevel ? 'btn tonal' : 'btn filled';
    }
    function latchConfirm(){
        if (latchPin < 0) return;
        fetchJson('/latchset', { method: 'POST', body: JSON.stringify({ pin: latchPin, level: latchPendingLevel, devMode: ioPanelDevMode, allowPwm: ioPanelAllowPwm }) })
            .then(d => {
                if (d.success){ toast('已锁存'); latchClose(); ioPanelConfirmCancel(); refreshIoPanel(); }
                else toast(d.error || '锁存失败');
            });
    }
    function latchClearDo(){
        if (ioPanelConfirmPin < 0) return;
        fetchJson('/latchclear', { method: 'POST', body: JSON.stringify({ pin: ioPanelConfirmPin }) })
            .then(d => {
                if (d.success){ toast('已解除锁存'); ioPanelConfirmCancel(); refreshIoPanel(); }
                else toast(d.error || '解除失败');
            });
    }

    )HTML";

#endif
