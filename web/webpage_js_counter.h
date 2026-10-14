// webpage_js_counter.h - 从 webpage.h 拆分而来
#ifndef ESPOGIC_WEBPAGE_JS_COUNTER_H
#define ESPOGIC_WEBPAGE_JS_COUNTER_H

static const char PAGE_JS_COUNTER[] PROGMEM = R"HTML(// ===== 计数器 =====
    let COUNTER = { tasks: [] };
    let counterEditingTask = -1;
    let counterPendingTriggerPin = -1;
    let counterPendingActionPin = -1;
    let counterPendingLevel = 1;
    let counterPendingHoldMode = 0;
    let counterTriggerDevMode = false;
    let counterTriggerAllowPwm = false;
    let counterActionDevMode = false;
    let counterActionAllowPwm = false;
    let counterActionAllowDup = false;
    let counterDelTarget = null;
    let counterPanelData = [];
    let counterPanelPolling = false;
    let counterPanelCountPwm = false;
    let counterPanelCountFlash = false;
    let counterClearPinTarget = -1;
    let counterPanelRendered = false;
    let counterPanelPrev = {};
    const COUNTER_HOLD_MAX_SEC = 604800;   // 锁存时长上限，与后端 config.h 的 COUNTER_HOLD_MAX_SEC 一致（168 小时）

    function fmtHold(sec){
        sec = Math.max(0, parseInt(sec) || 0);
        const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
        // 支持秒级时长后：不足 1 分显示"30秒"，带秒显示"1分30秒"，整分钟仍显示"1时0分"
        return (h > 0 ? (h + '时') : '') + ((h > 0 || m > 0 || s === 0) ? (m + '分') : '') + (s > 0 ? (s + '秒') : '');
    }
    function fmtRemain(sec){
        sec = Math.max(0, parseInt(sec) || 0);
        const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
        return h + '时' + m + '分' + s + '秒';
    }

    function loadCounterConfig(){
        return fetchJson('/countergetconfig').then(cfg => { COUNTER = cfg; counterRender(); }).catch(() => {});
    }

    function counterRender(){
        let html = '';
        (COUNTER.tasks || []).forEach((t, ti) => {
            let actionsHtml = '';
            (t.actions || []).forEach((a, ai) => {
                let holdHtml;
                if (a.holdMode === 1){
                    let remainHtml = '';
                    if (a.remain >= 0){
                        remainHtml = ' <span class="counter-remain" id="counter_remain_' + ti + '_' + ai + '">剩余 ' + fmtRemain(a.remain) + '</span>';
                    }
                    holdHtml = '锁存 ' + fmtHold(a.holdSec) + remainHtml;
                } else {
                    holdHtml = '修改';
                }
                actionsHtml += '<div class="comparator-item' + (a.noPerm ? ' comparator-item-noperm' : '') + '"><span class="comparator-tag">GPIO ' + a.pin + '</span>' +
                    '<span class="io-action ' + (a.level ? 'io-on' : 'io-off') + '">' + (a.level ? '高电平' : '低电平') + '</span>' +
                    '<span class="comparator-ref">' + holdHtml + '</span>' +
                    (a.noPerm ? '<span class="io-noperm-tag">无权限</span>' : '') +
                    '<button class="icon-btn comparator-del" onclick="event.stopPropagation();counterActionDel(' + ti + ',' + ai + ')">✕</button></div>';
            });
            const trigHtml = (t.triggerPin >= 0)
                ? ('<b>GPIO ' + t.triggerPin + '</b> · 反转 <b>' + t.threshold + '</b> 次触发 · 当前已反转 <b id="counter_trigcount_' + ti + '">' + t.triggerCount + '</b> 次')
                : '未设置';
            html += '<section class="card channel' + (t.enabled ? ' on' : '') + '">' +
                '<div class="channel-head">' +
                    '<div class="channel-title-wrap">' +
                        '<span class="channel-dot"></span>' +
                        '<button type="button" class="pin-chip" onclick="counterTaskEditOpen(' + ti + ')">' + esc(t.name) + ' <span class="edit-hint">重命名</span></button>' +
                    '</div>' +
                    '<div class="channel-actions">' +
                        '<button class="btn tonal" onclick="counterTaskDel(' + ti + ')">删除任务</button>' +
                        '<label class="switch-row" style="cursor:pointer;">' +
                            '<input type="checkbox" class="switch" ' + (t.enabled ? 'checked' : '') + ' onchange="counterToggle(' + ti + ', this)">' +
                        '</label>' +
                    '</div>' +
                '</div>' +
                '<div class="channel-status">' +
                    '<span>触发源 ' + trigHtml + '</span>' +
                    '<button class="btn filled" style="padding:6px 12px;font-size:12px;margin-left:auto;" onclick="counterTriggerOpen(' + ti + ')">设置触发源</button>' +
                '</div>' +
                '<div class="comparator-items">' + (actionsHtml || '<div class="comparator-empty">暂无输出动作，点击下方按钮添加</div>') + '</div>' +
                '<div class="channel-footer" style="justify-content:flex-start;">' +
                    '<button class="btn tonal" onclick="counterActionAddOpen(' + ti + ')">＋ 添加动作</button>' +
                '</div>' +
            '</section>';
        });
        $('counterTasks').innerHTML = html || '<div class="card io-empty">暂无计数器任务，点击上方「添加任务」创建</div>';
    }

    function counterToggle(ti, sw){
        fetchJson('/countertoggle', { method: 'POST', body: JSON.stringify({ index: ti }) }).then(d => {
            if (d.success){
                sw.checked = d.enabled;
                const card = sw.closest('.channel'); if (card) card.classList.toggle('on', d.enabled);
                toast(d.enabled ? '任务已开启' : '任务已关闭');
            } else {
                sw.checked = !sw.checked;
                toast(d.error || '操作失败');
            }
        });
    }

    // ===== 计量面板 =====
    function openCounterPanel(){
        $('counterPanelModal').classList.add('show');
        document.body.classList.add('modal-open');
        if (!counterPanelPolling){
            counterPanelPolling = true;
            refreshCounterPanel();
        }
    }
    function closeCounterPanel(){
        $('counterPanelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterPanelPolling = false;
    }
    function refreshCounterPanel(){
        fetchJson('/counterstatus').then(d => {
            if (!counterPanelPolling) return;
            counterPanelData = d.pins || [];
            counterPanelCountPwm = !!d.countPwm;
            counterPanelCountFlash = !!d.countFlash;
            const sw = $('counterPanelDevModeSwitch');
            if (sw) sw.checked = (counterPanelCountPwm || counterPanelCountFlash);
            renderCounterPanel();
            setTimeout(refreshCounterPanel, 1000);
        }).catch(() => { counterPanelPolling = false; });
    }
    function counterPanelDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const ps = $('counterPanelCountPwmSwitch'); if (ps) ps.checked = counterPanelCountPwm;
            const fs = $('counterPanelCountFlashSwitch'); if (fs) fs.checked = counterPanelCountFlash;
            $('counterPanelDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            counterPanelCountPwm = false;
            counterPanelCountFlash = false;
            counterDevModeSet(false, false);
        }
    }
    function counterPanelDevModeConfirm(){
        counterPanelCountPwm = $('counterPanelCountPwmSwitch') ? $('counterPanelCountPwmSwitch').checked : false;
        counterPanelCountFlash = $('counterPanelCountFlashSwitch') ? $('counterPanelCountFlashSwitch').checked : false;
        $('counterPanelDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterPanelDevModeSwitch'); if (sw) sw.checked = true;
        counterDevModeSet(counterPanelCountPwm, counterPanelCountFlash);
    }
    function counterPanelDevModeCancel(){
        $('counterPanelDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterPanelDevModeSwitch'); if (sw) sw.checked = false;
    }
    function counterDevModeSet(countPwm, countFlash){
        fetchJson('/counterdevmode', { method: 'POST', body: JSON.stringify({ countPwm: countPwm, countFlash: countFlash }) })
            .then(d => {
                if (d.success){ refreshCounterPanel(); }
                else toast(d.error || '设置失败');
            });
    }
    function counterClearPinOpen(pin){
        counterClearPinTarget = pin;
        $('counterClearPinDesc').textContent = '是否清除 GPIO ' + pin + ' 的切变计数？';
        $('counterClearPinModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterClearPinCancel(){
        $('counterClearPinModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterClearPinTarget = -1;
    }
    function counterClearPinConfirm(){
        if (counterClearPinTarget < 0) return;
        const pin = counterClearPinTarget;
        counterClearPinTarget = -1;
        $('counterClearPinModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        fetchJson('/counterclearpin', { method: 'POST', body: JSON.stringify({ pin: pin }) })
            .then(d => {
                if (d.success){ toast('已清除'); refreshCounterPanel(); }
                else toast(d.error || '清除失败');
            });
    }
    function buildCounterPanelItem(g){
        const disabled = (g.pwm && !counterPanelCountPwm) || (g.flash && !counterPanelCountFlash);
        const badgeCls = disabled ? 'badge-danger' : 'badge';
        const badgeText = disabled ? '停止计数' : (g.count + ' 次');
        let lvlHtml;
        if (g.level < 0){
            const tag = g.pwm ? 'PWM' : 'PSRAM';
            lvlHtml = '<span class="badge" style="margin-left:auto;background:rgba(79,209,197,.1);border-color:rgba(79,209,197,.32);color:#4fd1c5;">' + tag + '</span>';
        } else {
            const lvlCls = g.level ? 'lvl-high' : 'lvl-low';
            const lvlText = g.level ? '高' : '低';
            lvlHtml = '<span class="' + lvlCls + '" style="margin-left:auto;">' + lvlText + '</span>';
        }
        return '<div class="io-panel-item" id="cp_item_' + g.pin + '" onclick="counterClearPinOpen(' + g.pin + ')">' +
            '<span class="io-panel-name" style="flex:none;">GPIO ' + g.pin + '</span>' +
            '<span class="' + badgeCls + '">' + badgeText + '</span>' +
            lvlHtml +
        '</div>';
    }

    function renderCounterPanel(){
        const list = $('counterPanelList');
        if (!counterPanelData.length){
            list.innerHTML = '<div class="io-empty" style="grid-column:1 / -1;">无可用引脚</div>';
            counterPanelRendered = false;
            counterPanelPrev = {};
            return;
        }
        // 首次全量渲染；之后仅就地更新发生变化的那个引脚，其余引脚 DOM 不动，避免无关引脚闪烁
        if (!counterPanelRendered){
            counterPanelRendered = true;
            counterPanelPrev = {};
            list.innerHTML = counterPanelData.map(g => {
                counterPanelPrev[g.pin] = { level: g.level, count: g.count, pwm: g.pwm, flash: g.flash, countPwm: counterPanelCountPwm, countFlash: counterPanelCountFlash };
                return buildCounterPanelItem(g);
            }).join('');
            return;
        }
        counterPanelData.forEach(g => {
            const prev = counterPanelPrev[g.pin];
            const changed = !prev ||
                prev.level !== g.level ||
                prev.count !== g.count ||
                prev.pwm !== g.pwm ||
                prev.flash !== g.flash ||
                prev.countPwm !== counterPanelCountPwm ||
                prev.countFlash !== counterPanelCountFlash;
            if (changed){
                const item = $('cp_item_' + g.pin);
                if (item) item.outerHTML = buildCounterPanelItem(g);
                counterPanelPrev[g.pin] = { level: g.level, count: g.count, pwm: g.pwm, flash: g.flash, countPwm: counterPanelCountPwm, countFlash: counterPanelCountFlash };
            }
        });
    }
    function counterClear(){
        fetchJson('/counterclear', { method: 'POST', body: JSON.stringify({}) }).then(d => {
            if (d.success){ toast('已清零'); refreshCounterPanel(); }
            else toast(d.error || '操作失败');
        });
    }

    // ===== 任务 CRUD =====
    function counterTaskAddOpen(){
        counterEditingTask = -1;
        $('counterTaskTitle').textContent = '添加计数器任务';
        $('counterTaskName').value = '';
        $('counterTaskSaveBtn').textContent = '添加';
        $('counterTaskModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterTaskEditOpen(ti){
        counterEditingTask = ti;
        $('counterTaskTitle').textContent = '重命名计数器任务';
        $('counterTaskName').value = (COUNTER.tasks[ti] && COUNTER.tasks[ti].name) || '';
        $('counterTaskSaveBtn').textContent = '保存';
        $('counterTaskModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterTaskClose(){
        $('counterTaskModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterEditingTask = -1;
    }
    function counterTaskSave(){
        const name = $('counterTaskName').value.trim();
        const url = (counterEditingTask < 0) ? '/countertaskadd' : '/countertaskupdate';
        const body = (counterEditingTask < 0) ? { name: name } : { index: counterEditingTask, name: name };
        fetchJson(url, { method: 'POST', body: JSON.stringify(body) }).then(d => {
            if (d.success){ toast('任务已保存'); counterTaskClose(); loadCounterConfig(); }
            else toast(d.error || '保存失败');
        });
    }
    function counterTaskDel(ti){
        const t = COUNTER.tasks[ti];
        counterDelTarget = { type: 'task', ti: ti };
        $('counterDelDesc').textContent = '确认删除该计数器任务？' + (t ? '（名称：' + t.name + '）' : '');
        $('counterDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    // ===== 触发源 =====
    function counterTriggerOpen(ti){
        counterEditingTask = ti;
        counterPendingTriggerPin = (COUNTER.tasks[ti] && COUNTER.tasks[ti].triggerPin >= 0) ? COUNTER.tasks[ti].triggerPin : -1;
        $('counterThreshold').value = (COUNTER.tasks[ti] && COUNTER.tasks[ti].threshold) || 1;
        counterTriggerDevMode = false;
        counterTriggerAllowPwm = false;
        const sw = $('counterTriggerDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('counterTriggerAllowPwmSwitch'); if (ps) ps.checked = false;
        counterTriggerRenderGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('counterTriggerModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterTriggerClose(){
        $('counterTriggerModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterEditingTask = -1;
    }
    function counterTriggerSelect(p){
        counterPendingTriggerPin = p;
        // 触发源是"读"引脚：后端会拒绝锁存/手动中的引脚（PIN-2），这里先讲清（保存按钮同时被禁用）
        if (latchedPins.indexOf(p) >= 0) toast('注意：GPIO' + p + ' 处于调度面板锁存中，不能作为触发源（需先在调度面板解除锁存）');
        else if (counterTaskActionPins().indexOf(p) >= 0) toast('注意：GPIO' + p + ' 已是本任务的动作脚，不能同时作为触发源（请先删除该动作或换脚）');
        transitionModalHeight($('counterTriggerModal'), () => counterTriggerRenderGrid());
    }
    function counterTriggerUpdateSaveBtn(){
        const btn = $('counterTriggerSaveBtn');
        if (!btn) return;
        const p = counterPendingTriggerPin;
        if (p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const isPwm = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        btn.disabled = (latchedPins.indexOf(p) >= 0) || (counterTaskActionPins().indexOf(p) >= 0) || (isFlash && !counterTriggerDevMode) || (isPwm && !counterTriggerAllowPwm);   // 锁存脚/本任务动作脚不能作触发源（后端无条件拒）：灰可点、不可保存
    }
    function counterTriggerRenderGrid(){
        const grid = $('counterTriggerGrid');
        if (!CONFIG || !CONFIG.availablePins){ grid.innerHTML = ''; counterTriggerUpdateSaveBtn(); return; }
        const pwmPins = CONFIG.pwmPins || [];
        const current = counterPendingTriggerPin;
        grid.innerHTML = CONFIG.availablePins.map(p => {
            // 已锁存/手动：置灰只作"占用提示"，仍可点选（点了给 toast），真正的拦截在保存按钮（后端同样拒绝，PIN-2）
            if (latchedPins.indexOf(p) >= 0){
                // 选中后同时保留 current：既显示"已选"，又不丢掉灰（不可保存）
                const lcls = (p === current) ? 'pin-item pin-used current' : 'pin-item pin-used';
                return '<button type="button" class="' + lcls + '" title="该引脚已锁存，不能作为触发源；请先在调度面板解除锁存" onclick="counterTriggerSelect(' + p + ')">GPIO ' + p + '</button>';
            }
            // 本任务自己的动作脚：不能同时作触发源（后端无条件拒绝，PIN-6）——同样是"灰 + 可点 + 不可保存"
            if (counterTaskActionPins().indexOf(p) >= 0){
                const acls = (p === current) ? 'pin-item pin-used current' : 'pin-item pin-used';
                return '<button type="button" class="' + acls + '" title="该引脚已是本任务的动作脚，不能同时作为触发源" onclick="counterTriggerSelect(' + p + ')">GPIO ' + p + '</button>';
            }
            const isFlash = ioIsFlashPin(p);
            const isPwm = pwmPins.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : (isPwm ? 'pin-used' : ''));
            return '<button type="button" class="pin-item ' + cls + '" onclick="counterTriggerSelect(' + p + ')">GPIO ' + p + '</button>';
        }).join('');
        updatePinWarning(current, 'counterTriggerWarning');
        counterTriggerUpdateSaveBtn();
    }
    function counterTriggerDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;
            const ps = $('counterTriggerAllowPwmSwitch'); if (ps) ps.checked = false;
            $('counterTriggerDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            counterTriggerDevMode = false;
            counterTriggerAllowPwm = false;
            transitionModalHeight($('counterTriggerModal'), () => counterTriggerRenderGrid());
        }
    }
    function counterTriggerDevModeConfirm(){
        counterTriggerDevMode = true;
        counterTriggerAllowPwm = $('counterTriggerAllowPwmSwitch') ? $('counterTriggerAllowPwmSwitch').checked : false;
        $('counterTriggerDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterTriggerDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('counterTriggerModal'), () => counterTriggerRenderGrid());
    }
    function counterTriggerDevModeCancel(){
        $('counterTriggerDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterTriggerDevModeSwitch'); if (sw) sw.checked = false;
    }
    function counterTriggerSave(){
        if (counterEditingTask < 0){ toast('上下文无效'); return; }
        if (counterPendingTriggerPin < 0){ toast('请选择一个触发源引脚'); return; }
        const threshold = val('counterThreshold', 1, 1, 1000000);
        fetchJson('/countertriggerset', { method: 'POST', body: JSON.stringify({ index: counterEditingTask, pin: counterPendingTriggerPin, threshold: threshold, devMode: counterTriggerDevMode, allowPwm: counterTriggerAllowPwm }) })
            .then(d => {
                if (d.success){ toast('触发源已保存'); counterTriggerClose(); loadCounterConfig(); }
                else toast(d.error || '保存失败');
            });
    }

    // ===== 输出动作 =====
    function counterCompUsedPins(){
        const used = [];
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => used.push(a.pin));
            (g.inputs || []).forEach(i => { if (i.source === 2) used.push(i.refIndex); });
        });
        return used;
    }
    function counterOtherActionPins(){
        const used = [];
        (COUNTER.tasks || []).forEach((t, ti) => {
            if (ti === counterEditingTask) return;
            (t.actions || []).forEach(a => used.push(a.pin));
        });
        return used;
    }
    // 本任务已配置的输出动作引脚：与触发源互斥（后端无条件拒绝，PIN-6）
    function counterTaskActionPins(){
        const t = (COUNTER.tasks || [])[counterEditingTask];
        return (t && t.actions ? t.actions : []).map(a => a.pin);
    }
    function counterActionAddOpen(ti){
        counterEditingTask = ti;
        counterPendingActionPin = -1;
        counterPendingLevel = 1;
        counterPendingHoldMode = 0;
        counterActionDevMode = false;
        counterActionAllowPwm = false;
        counterActionAllowDup = false;
        const sw = $('counterActionDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('counterActionAllowPwmSwitch'); if (ps) ps.checked = false;
        const ds = $('counterActionAllowDupSwitch'); if (ds) ds.checked = false;
        $('counterActionTitle').textContent = '添加输出动作';
        $('counterHoldMin').value = 0;
        $('counterHoldSec').value = 0;
        counterActionUpdateLevelBtn();
        counterActionUpdateHoldBtn();
        counterActionRenderGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('counterActionModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterActionClose(){
        $('counterActionModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterEditingTask = -1;
    }
    function counterActionSelect(p){
        counterPendingActionPin = p;
        // 本任务已有的动作脚：不能重复添加（后端无条件拒，PIN-8）——先讲清，保存按钮同时被禁用
        if (counterTaskActionPins().indexOf(p) >= 0) toast('注意：GPIO' + p + ' 已是本任务的动作脚，不能重复添加');
        else if (latchedPins.indexOf(p) >= 0) toast('注意：GPIO' + p + ' 处于调度面板锁存中，该动作输出不正确');
        transitionModalHeight($('counterActionModal'), () => counterActionRenderGrid());
    }
    function counterActionSetLevel(l){
        counterPendingLevel = l;
        counterActionUpdateLevelBtn();
    }
    function counterActionUpdateLevelBtn(){
        $('counterLvlHigh').className = counterPendingLevel ? 'btn filled' : 'btn tonal';
        $('counterLvlLow').className = counterPendingLevel ? 'btn tonal' : 'btn filled';
    }
    function counterActionSetHold(m){
        counterPendingHoldMode = m;
        transitionModalHeight($('counterActionModal'), () => counterActionUpdateHoldBtn());
    }
    function counterActionUpdateHoldBtn(){
        $('counterHoldKeep').className = (counterPendingHoldMode === 0) ? 'btn filled' : 'btn tonal';
        $('counterHoldAutoOff').className = (counterPendingHoldMode === 1) ? 'btn filled' : 'btn tonal';
        $('counterHoldTimeRow').style.display = (counterPendingHoldMode === 1) ? '' : 'none';
    }
    function counterActionUpdateSaveBtn(){
        const btn = $('counterActionSaveBtn');
        if (!btn) return;
        const p = counterPendingActionPin;
        if (p < 0){ btn.disabled = true; return; }
        const triggerPin = (COUNTER.tasks[counterEditingTask] || {}).triggerPin;
        if (p === triggerPin){ btn.disabled = true; return; }
        if (counterTaskActionPins().indexOf(p) >= 0){ btn.disabled = true; return; }   // 本任务已有该动作脚（后端无条件拒，PIN-8）
        const isFlash = ioIsFlashPin(p);
        const isPwm = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        const isIo = (IO.groups || []).some(g => g.pin === p);
        const isComp = counterCompUsedPins().indexOf(p) >= 0;
        const isOther = counterOtherActionPins().indexOf(p) >= 0;
        btn.disabled = (isFlash && !counterActionDevMode) || (isPwm && !counterActionAllowPwm) || ((isIo || isComp || isOther) && !counterActionAllowDup);
    }
    function counterActionRenderGrid(){
        const grid = $('counterActionGrid');
        if (!CONFIG || !CONFIG.availablePins){ grid.innerHTML = ''; counterActionUpdateSaveBtn(); return; }
        const pwmPins = CONFIG.pwmPins || [];
        const ioPins = (IO.groups || []).map(g => g.pin);
        const compPins = counterCompUsedPins();
        const otherPins = counterOtherActionPins();
        const triggerPin = (COUNTER.tasks[counterEditingTask] || {}).triggerPin;
        const samePins = counterTaskActionPins();
        const current = counterPendingActionPin;
        grid.innerHTML = CONFIG.availablePins.map(p => {
            // 本任务已有的动作脚：不能重复添加（后端无条件拒绝，PIN-8）——同样是"灰 + 可点 + 不可保存"
            if (samePins.indexOf(p) >= 0){
                const scls = (p === current) ? 'pin-item pin-used current' : 'pin-item pin-used';
                return '<button type="button" class="' + scls + '" title="该引脚已是本任务的动作脚，不能重复添加" onclick="counterActionSelect(' + p + ')">GPIO ' + p + '</button>';
            }
            const isFlash = ioIsFlashPin(p);
            const isTrigger = (p === triggerPin);
            const isUsed = pwmPins.indexOf(p) >= 0 || ioPins.indexOf(p) >= 0 || compPins.indexOf(p) >= 0 || otherPins.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : (isTrigger || isUsed ? 'pin-used' : ''));
            return '<button type="button" class="pin-item ' + cls + '" onclick="counterActionSelect(' + p + ')">GPIO ' + p + '</button>';
        }).join('');
        updatePinWarning(current, 'counterActionWarning');
        counterActionUpdateSaveBtn();
    }
    function counterActionDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;
            const ps = $('counterActionAllowPwmSwitch'); if (ps) ps.checked = false;
            const ds = $('counterActionAllowDupSwitch'); if (ds) ds.checked = false;
            $('counterActionDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            counterActionDevMode = false;
            counterActionAllowPwm = false;
            counterActionAllowDup = false;
            transitionModalHeight($('counterActionModal'), () => counterActionRenderGrid());
        }
    }
    function counterActionDevModeConfirm(){
        counterActionDevMode = true;
        counterActionAllowPwm = $('counterActionAllowPwmSwitch') ? $('counterActionAllowPwmSwitch').checked : false;
        counterActionAllowDup = $('counterActionAllowDupSwitch') ? $('counterActionAllowDupSwitch').checked : false;
        $('counterActionDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterActionDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('counterActionModal'), () => counterActionRenderGrid());
    }
    function counterActionDevModeCancel(){
        $('counterActionDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('counterActionDevModeSwitch'); if (sw) sw.checked = false;
    }
    function counterActionSave(){
        if (counterEditingTask < 0){ toast('上下文无效'); return; }
        if (counterPendingActionPin < 0){ toast('请选择一个输出引脚'); return; }
        let holdSec = 0;
        if (counterPendingHoldMode === 1){
            // 分/秒输入：分 ≤ 10079、秒 ≤ 60 → 合计上限 10079×60 + 60 = 604800 秒，正好 168 小时（输入范围本身就取不到超限值）
            holdSec = val('counterHoldMin', 0, 0, 10079) * 60 + val('counterHoldSec', 0, 0, 60);
            if (holdSec > COUNTER_HOLD_MAX_SEC) holdSec = COUNTER_HOLD_MAX_SEC;   // 兜底不变式（当前输入范围取不到）
            if (holdSec <= 0){ toast('请设置锁存时长'); return; }
        }
        fetchJson('/counteractionadd', { method: 'POST', body: JSON.stringify({ taskIndex: counterEditingTask, pin: counterPendingActionPin, level: counterPendingLevel, holdMode: counterPendingHoldMode, holdSec: holdSec, devMode: counterActionDevMode, allowPwm: counterActionAllowPwm, allowDup: counterActionAllowDup }) })
            .then(d => {
                if (d.success){ toast('动作已添加'); counterActionClose(); loadCounterConfig(); }
                else toast(d.error || '添加失败');
            });
    }

    function counterHoldHelp(){
        $('counterHoldHelpModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function counterHoldHelpClose(){
        $('counterHoldHelpModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function counterActionDel(ti, ai){
        counterDelTarget = { type: 'action', ti: ti, ai: ai };
        $('counterDelDesc').textContent = '确认删除此输出动作？';
        $('counterDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    // ===== 删除确认 =====
    function counterDelCancel(){
        $('counterDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        counterDelTarget = null;
    }
    function counterDelConfirm(){
        if (!counterDelTarget) return;
        const t = counterDelTarget;
        counterDelTarget = null;
        $('counterDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        let url = '', body = {};
        if (t.type === 'task'){ url = '/countertaskdel'; body = { index: t.ti }; }
        else { url = '/counteractiondel'; body = { taskIndex: t.ti, actionIndex: t.ai }; }
        fetchJson(url, { method: 'POST', body: JSON.stringify(body) }).then(d => {
            if (d.success){ toast('已删除'); loadCounterConfig(); }
            else toast(d.error || '删除失败');
        });
    }

    // ===== 启动 =====
    loadConfig().then(() => {
        // IO 与计数器互不依赖，并行加载；比较器依赖两者（引用其名称/引脚），故放到两者之后
        Promise.all([loadIoConfig(), loadCounterConfig()]).then(() => loadComparatorConfig());
        loadStatus();
    });
    function refreshCounterRemain(){
        fetchJson('/countergetconfig').then(cfg => {
            (cfg.tasks || []).forEach((t, ti) => {
                (t.actions || []).forEach((a, ai) => {
                    const el = $('counter_remain_' + ti + '_' + ai);
                    if (el){
                        el.textContent = (a.holdMode === 1 && a.remain >= 0) ? ('剩余 ' + fmtRemain(a.remain)) : '';
                    }
                });
                const tc = $('counter_trigcount_' + ti);
                if (tc && t.triggerPin >= 0){
                    tc.textContent = t.triggerCount;
                }
            });
        }).catch(() => {});
    }
    setInterval(refreshCounterRemain, 1000);
    setInterval(loadStatus, 500);
    )HTML";

#endif
