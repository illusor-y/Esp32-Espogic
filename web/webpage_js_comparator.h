// webpage_js_comparator.h - 从 webpage.h 拆分而来
#ifndef SMARTGLASS_WEBPAGE_JS_COMPARATOR_H
#define SMARTGLASS_WEBPAGE_JS_COMPARATOR_H

static const char PAGE_JS_COMPARATOR[] PROGMEM = R"HTML(// ===== 比较器 =====
    let COMPARATOR = { gates: [] };
    let comparatorEditingGate = -1;     // 正在编辑的门 index（-1=新增）
    let comparatorGateContext = -1;     // 当前操作的门 index（输入/输出增删的上下文）
    let comparatorPendingSource = 0;    // 0=引用序列器, 1=引用计数器, 2=外部输入
    let comparatorPendingRef = -1;      // 序列器=组索引, 计数器=任务索引, 外部=GPIO
    let comparatorPendingAction = -1;   // 计数器=动作索引
    let comparatorPendingOutputPin = -1;
    let comparatorPendingSourceLevel = 0;   // 源IO电平（复位电平）
    let comparatorPendingActionLevel = 1;   // 行为电平（满足时输出）
    let comparatorPendingInputLevel = 1;  // 输入条件期望电平：0=低, 1=高
    let comparatorPendingMode = 0;        // 逻辑类型：0=AND, 1=OR, 2=NOR, 3=NAND
    let comparatorDevMode = false;      // 开发模式：放开 Flash 禁用（输入）
    let comparatorAllowPwm = false;     // 允许选中 PWM 占用引脚（输入）
    let comparatorAllowDup = false;     // 允许重复选中引脚（输入）
    let comparatorOutputDevMode = false;      // 开发模式（输出）
    let comparatorOutputAllowPwm = false;     // 允许选中 PWM 占用引脚（输出）
    let comparatorOutputAllowDup = false;     // 允许重复选中引脚（输出）
    let comparatorDelTarget = null;

    function loadComparatorConfig(){
        return fetchJson('/comparatorgetconfig').then(cfg => { COMPARATOR = cfg; comparatorRender(); }).catch(() => {});
    }

    function comparatorIoGroupName(gi){
        const g = IO.groups && IO.groups[gi];
        if (g) return g.name + ' · GPIO ' + g.pin;
        return '无效引用';
    }

    function comparatorModeName(m){
        return ['与', '或', '或非', '与非'][m] || '与';
    }
    function comparatorModeEn(m){
        return ['AND', 'OR', 'NOR', 'NAND'][m] || 'AND';
    }
    function comparatorModeTriggerText(m){
        return ['全部达成触发', '任一达成触发', '无一达成触发', '任一不达成触发'][m] || '全部达成触发';
    }
    function comparatorModeDesc(m){
        return [
            '条件全部满足时触发输出（至少 1 个条件）',
            '任一条件满足即触发输出（至少 2 个条件）',
            '所有条件都不满足时才触发输出（至少 1 个条件）',
            '只要有一个条件不满足即触发输出（至少 2 个条件）'
        ][m] || '条件全部满足时触发输出（至少 1 个条件）';
    }

    function comparatorCounterActionName(ti, ai){
        const t = COUNTER.tasks && COUNTER.tasks[ti];
        if (t && t.actions && t.actions[ai]){
            return t.name + ' · GPIO ' + t.actions[ai].pin;
        }
        return '无效引用';
    }

    function comparatorRender(){
        let html = '';
        (COMPARATOR.gates || []).forEach((g, gi) => {
            let inputsHtml = '';
            (g.inputs || []).forEach((inp, ii) => {
                let tag, refText;
                if (inp.source === 2){
                    tag = '<span class="comparator-tag ext">外部输入</span>';
                    refText = 'GPIO ' + inp.refIndex;
                } else if (inp.source === 1){
                    tag = '<span class="comparator-tag">计数器</span>';
                    refText = comparatorCounterActionName(inp.refIndex, inp.actionIndex);
                } else {
                    tag = '<span class="comparator-tag">序列器</span>';
                    refText = comparatorIoGroupName(inp.refIndex);
                }
                // 期望电平只用一个字（高/低）：与右半的"源:高 / 行为:低"保持一致，四个字太长会挤掉引用名
                const lvlTag = '<span class="io-action ' + (inp.expectedLevel ? 'io-on' : 'io-off') + '">' + (inp.expectedLevel ? '高' : '低') + '</span>';
                inputsHtml += '<div class="comparator-item">' + tag + lvlTag +
                    '<span class="comparator-ref">' + esc(refText) + '</span>' +
                    '<button class="icon-btn comparator-del" onclick="event.stopPropagation();comparatorInputDel(' + gi + ',' + ii + ')">✕</button></div>';
            });
            let actionsHtml = '';
            (g.actions || []).forEach((a, ai) => {
                actionsHtml += '<div class="comparator-item' + (a.noPerm ? ' comparator-item-noperm' : '') + '"><span class="comparator-tag">GPIO ' + a.pin + '</span>' +
                    '<span class="io-action ' + (a.sourceLevel ? 'io-on' : 'io-off') + '">源:' + (a.sourceLevel ? '高' : '低') + '</span>' +
                    '<span class="io-action ' + (a.actionLevel ? 'io-on' : 'io-off') + '">行为:' + (a.actionLevel ? '高' : '低') + '</span>' +
                    (a.noPerm ? '<span class="io-noperm-tag">无权限</span>' : '') +
                    '<button class="icon-btn comparator-del" onclick="event.stopPropagation();comparatorOutputDel(' + gi + ',' + ai + ')">✕</button></div>';
            });
            html += '<section class="card channel' + (g.enabled ? ' on' : '') + '">' +
                '<div class="channel-head">' +
                    '<div class="channel-title-wrap">' +
                        '<span class="channel-dot"></span>' +
                        '<button type="button" class="pin-chip" onclick="comparatorGateEditOpen(' + gi + ')">' + esc(g.name) + ' <span class="edit-hint">重命名</span></button>' +
                        '<button type="button" class="mode-chip" onclick="comparatorGateEditOpen(' + gi + ')" title="点击修改逻辑类型">' + comparatorModeName(g.mode) + '</button>' +
                    '</div>' +
                    '<div class="channel-actions">' +
                        '<button class="btn tonal" onclick="comparatorGateDel(' + gi + ')">删除任务</button>' +
                        '<label class="switch-row" style="cursor:pointer;">' +
                            '<input type="checkbox" class="switch" ' + (g.enabled ? 'checked' : '') + ' onchange="comparatorToggle(' + gi + ', this)">' +
                        '</label>' +
                    '</div>' +
                '</div>' +
                '<div class="comparator-grid">' +
                    '<div class="comparator-half">' +
                        '<div class="comparator-half-title"><span>左半 · 输入条件（' + comparatorModeEn(g.mode) + '）</span><button class="icon-btn" onclick="comparatorInputAddOpen(' + gi + ')">＋</button></div>' +
                        '<div class="comparator-items">' + (inputsHtml || '<div class="comparator-empty">暂无输入，点击 ＋ 添加</div>') + '</div>' +
                    '</div>' +
                    '<div class="comparator-bridge"><span class="arr">→</span><span class="bridge-label">' + comparatorModeTriggerText(g.mode) + '</span></div>' +
                    '<div class="comparator-half">' +
                        '<div class="comparator-half-title"><span>右半 · 输出动作</span><button class="icon-btn" onclick="comparatorOutputAddOpen(' + gi + ')">＋</button></div>' +
                        '<div class="comparator-items">' + (actionsHtml || '<div class="comparator-empty">暂无输出，点击 ＋ 添加</div>') + '</div>' +
                    '</div>' +
                '</div>' +
            '</section>';
        });
        $('comparatorGates').innerHTML = html || '<div class="card io-empty">暂无比较器任务，点击上方「添加任务」创建</div>';
    }

    function comparatorToggle(gi, sw){
        fetchJson('/comparatortoggle', { method: 'POST', body: JSON.stringify({ index: gi }) }).then(d => {
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

    // ===== 门 CRUD =====
    function comparatorGateAddOpen(){
        comparatorEditingGate = -1;
        comparatorPendingMode = 0;
        $('comparatorGateTitle').textContent = '添加比较器任务';
        $('comparatorGateName').value = '';
        $('comparatorGateSaveBtn').textContent = '添加';
        comparatorGateUpdateModeBtn();
        $('comparatorGateModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function comparatorGateEditOpen(gi){
        comparatorEditingGate = gi;
        comparatorPendingMode = (COMPARATOR.gates[gi] && COMPARATOR.gates[gi].mode) || 0;
        $('comparatorGateTitle').textContent = '重命名比较器任务';
        $('comparatorGateName').value = (COMPARATOR.gates[gi] && COMPARATOR.gates[gi].name) || '';
        $('comparatorGateSaveBtn').textContent = '保存';
        comparatorGateUpdateModeBtn();
        $('comparatorGateModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function comparatorGateSetMode(m){
        comparatorPendingMode = m;
        comparatorGateUpdateModeBtn();
    }
    function comparatorGateUpdateModeBtn(){
        $('comparatorModeAnd').className = (comparatorPendingMode === 0) ? 'btn filled' : 'btn tonal';
        $('comparatorModeOr').className = (comparatorPendingMode === 1) ? 'btn filled' : 'btn tonal';
        $('comparatorModeNor').className = (comparatorPendingMode === 2) ? 'btn filled' : 'btn tonal';
        $('comparatorModeNand').className = (comparatorPendingMode === 3) ? 'btn filled' : 'btn tonal';
        $('comparatorModeDesc').textContent = comparatorModeDesc(comparatorPendingMode);
    }
    function comparatorGateClose(){
        $('comparatorGateModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        comparatorEditingGate = -1;
    }
    function comparatorGateSave(){
        const name = $('comparatorGateName').value.trim();
        if (comparatorEditingGate < 0){
            fetchJson('/comparatorgateadd', { method: 'POST', body: JSON.stringify({ name: name, mode: comparatorPendingMode }) }).then(d => {
                if (d.success){ toast('任务已添加'); comparatorGateClose(); loadComparatorConfig(); }
                else toast(d.error || '添加失败');
            });
            return;
        }
        const gi = comparatorEditingGate;
        const old = COMPARATOR.gates[gi];
        const nameChanged = !old || old.name !== name;
        const modeChanged = !old || old.mode !== comparatorPendingMode;
        let p = Promise.resolve();
        if (nameChanged) p = p.then(() => fetchJson('/comparatorgateupdate', { method: 'POST', body: JSON.stringify({ index: gi, name: name }) }));
        if (modeChanged) p = p.then(() => fetchJson('/comparatorgatemode', { method: 'POST', body: JSON.stringify({ index: gi, mode: comparatorPendingMode }) }));
        p.then(() => { toast('任务已保存'); comparatorGateClose(); loadComparatorConfig(); })
         .catch(() => { toast('保存失败'); });
    }
    function comparatorGateDel(gi){
        const g = COMPARATOR.gates[gi];
        comparatorDelTarget = { type: 'gate', gi: gi };
        $('comparatorDelDesc').textContent = '确认删除该比较器任务？' + (g ? '（名称：' + g.name + '）' : '');
        $('comparatorDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    // ===== 输入 CRUD =====
    function comparatorInputAddOpen(gi){
        comparatorGateContext = gi;
        comparatorPendingSource = 0;
        comparatorPendingRef = -1;
        comparatorPendingAction = -1;
        comparatorPendingInputLevel = 1;
        comparatorDevMode = false;
        comparatorAllowPwm = false;
        comparatorAllowDup = false;
        const sw = $('comparatorDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('comparatorAllowPwmSwitch'); if (ps) ps.checked = false;
        const ds = $('comparatorAllowDupSwitch'); if (ds) ds.checked = false;
        $('comparatorSrcIo').className = 'btn filled';
        $('comparatorSrcCounter').className = 'btn tonal';
        $('comparatorSrcExternal').className = 'btn tonal';
        comparatorInputUpdateLevelBtn();
        comparatorInputRenderGrid();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('comparatorInputModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function comparatorInputClose(){
        $('comparatorInputModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        comparatorGateContext = -1;
    }
    function comparatorInputSetSource(src){
        comparatorPendingSource = src;
        comparatorPendingRef = -1;
        comparatorPendingAction = -1;
        $('comparatorSrcIo').className = (src === 0) ? 'btn filled' : 'btn tonal';
        $('comparatorSrcCounter').className = (src === 1) ? 'btn filled' : 'btn tonal';
        $('comparatorSrcExternal').className = (src === 2) ? 'btn filled' : 'btn tonal';
        transitionModalHeight($('comparatorInputModal'), () => comparatorInputRenderGrid());
    }
    function comparatorInputSetLevel(l){
        comparatorPendingInputLevel = l;
        comparatorInputUpdateLevelBtn();
    }
    function comparatorInputUpdateLevelBtn(){
        $('comparatorInputLvlHigh').className = comparatorPendingInputLevel ? 'btn filled' : 'btn tonal';
        $('comparatorInputLvlLow').className = comparatorPendingInputLevel ? 'btn tonal' : 'btn filled';
    }
    function comparatorDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const ps = $('comparatorAllowPwmSwitch'); if (ps) ps.checked = false;
            const ds = $('comparatorAllowDupSwitch'); if (ds) ds.checked = false;
            $('comparatorDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            comparatorDevMode = false;
            comparatorAllowPwm = false;
            comparatorAllowDup = false;
            transitionModalHeight($('comparatorInputModal'), () => comparatorInputRenderGrid());
        }
    }
    function comparatorDevModeConfirm(){
        comparatorDevMode = true;
        comparatorAllowPwm = $('comparatorAllowPwmSwitch') ? $('comparatorAllowPwmSwitch').checked : false;
        comparatorAllowDup = $('comparatorAllowDupSwitch') ? $('comparatorAllowDupSwitch').checked : false;
        $('comparatorDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('comparatorDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('comparatorInputModal'), () => comparatorInputRenderGrid());
    }
    function comparatorDevModeCancel(){
        $('comparatorDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('comparatorDevModeSwitch'); if (sw) sw.checked = false;
    }
    function comparatorExternalUsedPins(){
        const used = [];
        (COMPARATOR.gates || []).forEach(g => (g.actions || []).forEach(a => { used.push(a.pin); }));
        return used;
    }
    // 计数器动作占用的引脚：比较器的输入/输出都不能与它们同脚（后端 `!allowDup` 时拒绝，PIN-6）
    function comparatorCounterUsedPins(){
        const used = [];
        (COUNTER.tasks || []).forEach(t => (t.actions || []).forEach(a => used.push(a.pin)));
        return used;
    }
    function comparatorInputRenderGrid(){
        const grid = $('comparatorInputGrid');
        if (comparatorPendingSource === 0){
            // 引用序列器组
            const groups = IO.groups || [];
            let items = '';
            if (!groups.length){
                grid.innerHTML = '<div class="comparator-empty">暂无序列器，请先在「序列器」中创建</div>';
                comparatorInputUpdateSaveBtn();
                return;
            }
            groups.forEach((g, gi) => {
                const cls = (comparatorPendingRef === gi) ? 'current' : '';
                items += '<button type="button" class="pin-item pin-item-block ' + cls + '" onclick="comparatorInputSelect(' + gi + ')">' + esc(g.name) + ' · GPIO' + g.pin + '</button>';
            });
            grid.innerHTML = items;
            comparatorInputUpdateSaveBtn();
            return;
        }
        if (comparatorPendingSource === 1){
            // 引用计数器动作
            const tasks = COUNTER.tasks || [];
            let items = '';
            let any = false;
            tasks.forEach((t, ti) => {
                (t.actions || []).forEach((a, ai) => {
                    any = true;
                    const cls = (comparatorPendingRef === ti && comparatorPendingAction === ai) ? 'current' : '';
                    items += '<button type="button" class="pin-item pin-item-block ' + cls + '" onclick="comparatorInputSelectCounter(' + ti + ',' + ai + ')">' + esc(t.name) + ' · GPIO ' + a.pin + ' · ' + (a.level ? '高电平' : '低电平') + '</button>';
                });
            });
            grid.innerHTML = any ? items : '<div class="comparator-empty">暂无计数器动作，请先在「计数器」中添加</div>';
            comparatorInputUpdateSaveBtn();
            return;
        }
        // 外部输入：引脚网格（对齐序列器：Flash 标红、占用标灰、风险提示、保存按钮禁用）
        if (!CONFIG || !CONFIG.availablePins){ grid.innerHTML = ''; comparatorInputUpdateSaveBtn(); return; }
        const pwmPins = CONFIG.pwmPins || [];
        const ioPins = (IO.groups || []).map(g => g.pin);
        const outPins = comparatorExternalUsedPins();
        const cntPins = comparatorCounterUsedPins();   // 计数器动作脚：也属占用（PIN-6）
        const current = comparatorPendingRef;
        let items = '';
        CONFIG.availablePins.forEach(p => {
            // 已锁存：置灰只作"占用提示"，仍可点选（点了给 toast），真正的拦截在保存按钮（后端同样拒绝，PIN-2）
            if (latchedPins.indexOf(p) >= 0){
                // 选中后同时保留 current：既显示"已选"，又不丢掉灰（不可保存）
                const lcls = (p === current) ? 'pin-item pin-used current' : 'pin-item pin-used';
                items += '<button type="button" class="' + lcls + '" title="该引脚已锁存，不能作为外部输入；请先在调度面板解除锁存" onclick="comparatorInputSelect(' + p + ')">GPIO ' + p + '</button>';
                return;
            }
            const isFlash = ioIsFlashPin(p);
            const isUsed = pwmPins.indexOf(p) >= 0 || ioPins.indexOf(p) >= 0 || outPins.indexOf(p) >= 0 || cntPins.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : (isUsed ? 'pin-used' : ''));
            items += '<button type="button" class="pin-item ' + cls + '" onclick="comparatorInputSelect(' + p + ')">GPIO ' + p + '</button>';
        });
        grid.innerHTML = items;
        updatePinWarning(current, 'comparatorInputWarning');
        comparatorInputUpdateSaveBtn();
    }
    function comparatorInputUpdateSaveBtn(){
        const btn = $('comparatorInputSaveBtn');
        if (!btn) return;
        const p = comparatorPendingRef;
        if (comparatorPendingSource === 0){
            btn.disabled = (p < 0);   // 引用序列器组：选中即可保存
            return;
        }
        if (comparatorPendingSource === 1){
            btn.disabled = (p < 0 || comparatorPendingAction < 0);   // 引用计数器动作
            return;
        }
        if (p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const isPwm = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        const isIo = (IO.groups || []).some(g => g.pin === p);
        const isOut = comparatorExternalUsedPins().indexOf(p) >= 0;
        const isCnt = comparatorCounterUsedPins().indexOf(p) >= 0;   // 计数器动作占用（PIN-6）
        // 锁存脚不能作外部输入（后端必拒）：网格里灰但可点，靠这里禁用保存
        btn.disabled = (latchedPins.indexOf(p) >= 0) || (isFlash && !comparatorDevMode) || (isPwm && !comparatorAllowPwm) || ((isIo || isOut || isCnt) && !comparatorAllowDup);
    }
    function comparatorInputSelect(ref){
        comparatorPendingRef = ref;
        // 只有"外部输入"分支（source=2）传的是 GPIO；锁存/手动中的引脚会被后端拒（PIN-2），先讲清（保存按钮同时被禁用）
        if (comparatorPendingSource === 2 && latchedPins.indexOf(ref) >= 0) toast('注意：GPIO' + ref + ' 处于调度面板锁存中，不能作为外部输入（需先在调度面板解除锁存）');
        else if (comparatorPendingSource === 2 && comparatorCounterUsedPins().indexOf(ref) >= 0) toast('注意：GPIO' + ref + ' 已被计数器动作占用');
        transitionModalHeight($('comparatorInputModal'), () => comparatorInputRenderGrid());
    }
    function comparatorInputSelectCounter(ti, ai){
        comparatorPendingRef = ti;
        comparatorPendingAction = ai;
        transitionModalHeight($('comparatorInputModal'), () => comparatorInputRenderGrid());
    }
    function comparatorInputSave(){
        if (comparatorGateContext < 0){ toast('上下文无效'); return; }
        if (comparatorPendingRef < 0){ toast('请选择一个输入来源'); return; }
        fetchJson('/comparatorinputadd', { method: 'POST', body: JSON.stringify({ gateIndex: comparatorGateContext, source: comparatorPendingSource, refIndex: comparatorPendingRef, actionIndex: comparatorPendingAction, expectedLevel: comparatorPendingInputLevel, devMode: comparatorDevMode, allowPwm: comparatorAllowPwm, allowDup: comparatorAllowDup }) })
            .then(d => {
                if (d.success){ toast('输入已添加'); comparatorInputClose(); loadComparatorConfig(); }
                else toast(d.error || '添加失败');
            });
    }
    function comparatorInputDel(gi, ii){
        comparatorDelTarget = { type: 'input', gi: gi, ii: ii };
        $('comparatorDelDesc').textContent = '确认删除此输入条件？';
        $('comparatorDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    // ===== 输出 CRUD =====
    function comparatorOutputAddOpen(gi){
        comparatorGateContext = gi;
        comparatorPendingOutputPin = -1;
        comparatorPendingSourceLevel = 0;
        comparatorPendingActionLevel = 1;
        comparatorOutputDevMode = false;
        comparatorOutputAllowPwm = false;
        comparatorOutputAllowDup = false;
        const sw = $('comparatorOutputDevModeSwitch'); if (sw) sw.checked = false;
        const ps = $('comparatorOutputAllowPwmSwitch'); if (ps) ps.checked = false;
        const ds = $('comparatorOutputAllowDupSwitch'); if (ds) ds.checked = false;
        comparatorOutputRenderGrid();
        comparatorOutputUpdateLevelBtn();
        refreshLatchedPins();   // 拉取锁存状态（选中锁存脚时 toast 提示）
        $('comparatorOutputModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function comparatorOutputClose(){
        $('comparatorOutputModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        comparatorGateContext = -1;
    }
    function comparatorOutputHelpOpen(){
        $('comparatorOutputHelpModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function comparatorOutputHelpClose(){
        $('comparatorOutputHelpModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function comparatorOutputSetSourceLevel(l){
        comparatorPendingSourceLevel = l;
        comparatorOutputUpdateLevelBtn();
    }
    function comparatorOutputSetActionLevel(l){
        comparatorPendingActionLevel = l;
        comparatorOutputUpdateLevelBtn();
    }
    function comparatorOutputUpdateLevelBtn(){
        $('comparatorSrcLvlHigh').className = comparatorPendingSourceLevel ? 'btn filled' : 'btn tonal';
        $('comparatorSrcLvlLow').className = comparatorPendingSourceLevel ? 'btn tonal' : 'btn filled';
        $('comparatorActLvlHigh').className = comparatorPendingActionLevel ? 'btn filled' : 'btn tonal';
        $('comparatorActLvlLow').className = comparatorPendingActionLevel ? 'btn tonal' : 'btn filled';
    }
    function comparatorOutputDevModeToggle(sw){
        if (sw.checked){
            sw.checked = false;   // 先复位，确认后再真正开启
            const ps = $('comparatorOutputAllowPwmSwitch'); if (ps) ps.checked = false;
            const ds = $('comparatorOutputAllowDupSwitch'); if (ds) ds.checked = false;
            $('comparatorOutputDevModeModal').classList.add('show');
            document.body.classList.add('modal-open');
        } else {
            comparatorOutputDevMode = false;
            comparatorOutputAllowPwm = false;
            comparatorOutputAllowDup = false;
            transitionModalHeight($('comparatorOutputModal'), () => comparatorOutputRenderGrid());
        }
    }
    function comparatorOutputDevModeConfirm(){
        comparatorOutputDevMode = true;
        comparatorOutputAllowPwm = $('comparatorOutputAllowPwmSwitch') ? $('comparatorOutputAllowPwmSwitch').checked : false;
        comparatorOutputAllowDup = $('comparatorOutputAllowDupSwitch') ? $('comparatorOutputAllowDupSwitch').checked : false;
        $('comparatorOutputDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('comparatorOutputDevModeSwitch'); if (sw) sw.checked = true;
        transitionModalHeight($('comparatorOutputModal'), () => comparatorOutputRenderGrid());
    }
    function comparatorOutputDevModeCancel(){
        $('comparatorOutputDevModeModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        const sw = $('comparatorOutputDevModeSwitch'); if (sw) sw.checked = false;
    }
    function comparatorOutputRenderGrid(){
        const grid = $('comparatorOutputGrid');
        if (!CONFIG || !CONFIG.availablePins){ grid.innerHTML = ''; comparatorOutputUpdateSaveBtn(); return; }
        const pwmPins = CONFIG.pwmPins || [];
        const ioPins = (IO.groups || []).map(g => g.pin);
        const used = [];
        (COMPARATOR.gates || []).forEach((g) => {
            (g.actions || []).forEach(a => { used.push(a.pin); });
            (g.inputs || []).forEach(i => { if (i.source === 2) used.push(i.refIndex); });
        });
        comparatorCounterUsedPins().forEach(x => used.push(x));   // 计数器动作脚（PIN-6）
        const current = comparatorPendingOutputPin;
        let items = '';
        CONFIG.availablePins.forEach(p => {
            const isFlash = ioIsFlashPin(p);
            const isUsed = pwmPins.indexOf(p) >= 0 || ioPins.indexOf(p) >= 0 || used.indexOf(p) >= 0;
            const cls = (p === current) ? 'current' : (isFlash ? 'pin-danger' : (isUsed ? 'pin-used' : ''));
            items += '<button type="button" class="pin-item ' + cls + '" onclick="comparatorOutputSelect(' + p + ')">GPIO ' + p + '</button>';
        });
        grid.innerHTML = items;
        updatePinWarning(current, 'comparatorOutputWarning');
        comparatorOutputUpdateSaveBtn();
    }
    function comparatorOutputUpdateSaveBtn(){
        const btn = $('comparatorOutputSaveBtn');
        if (!btn) return;
        const p = comparatorPendingOutputPin;
        if (p < 0){ btn.disabled = true; return; }
        const isFlash = ioIsFlashPin(p);
        const isPwm = (CONFIG.pwmPins || []).indexOf(p) >= 0;
        const isIo = (IO.groups || []).some(g => g.pin === p);
        let isComparatorUsed = false;
        (COMPARATOR.gates || []).forEach(g => {
            (g.actions || []).forEach(a => { if (a.pin === p) isComparatorUsed = true; });
            (g.inputs || []).forEach(i => { if (i.source === 2 && i.refIndex === p) isComparatorUsed = true; });
        });
        const isCnt = comparatorCounterUsedPins().indexOf(p) >= 0;   // 计数器动作占用（PIN-6）
        btn.disabled = (isFlash && !comparatorOutputDevMode) || (isPwm && !comparatorOutputAllowPwm) || ((isIo || isComparatorUsed || isCnt) && !comparatorOutputAllowDup);
    }
    function comparatorOutputSelect(p){
        comparatorPendingOutputPin = p;
        if (latchedPins.indexOf(p) >= 0) toast('注意：GPIO' + p + ' 处于调度面板锁存中，该动作输出不正确');
        else if (comparatorCounterUsedPins().indexOf(p) >= 0) toast('注意：GPIO' + p + ' 已被计数器动作占用');
        comparatorOutputRenderGrid();
    }
    function comparatorOutputSave(){
        if (comparatorGateContext < 0){ toast('上下文无效'); return; }
        if (comparatorPendingOutputPin < 0){ toast('请选择一个输出引脚'); return; }
        fetchJson('/comparatoroutputadd', { method: 'POST', body: JSON.stringify({ gateIndex: comparatorGateContext, pin: comparatorPendingOutputPin, sourceLevel: comparatorPendingSourceLevel, actionLevel: comparatorPendingActionLevel, devMode: comparatorOutputDevMode, allowPwm: comparatorOutputAllowPwm, allowDup: comparatorOutputAllowDup }) })
            .then(d => {
                if (d.success){ toast('输出已添加'); comparatorOutputClose(); loadComparatorConfig(); }
                else toast(d.error || '添加失败');
            });
    }
    function comparatorOutputDel(gi, ai){
        comparatorDelTarget = { type: 'output', gi: gi, ai: ai };
        $('comparatorDelDesc').textContent = '确认删除此输出动作？';
        $('comparatorDelModal').classList.add('show');
        document.body.classList.add('modal-open');
    }

    // ===== 删除确认 =====
    function comparatorDelCancel(){
        $('comparatorDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        comparatorDelTarget = null;
    }
    function comparatorDelConfirm(){
        if (!comparatorDelTarget) return;
        const t = comparatorDelTarget;
        comparatorDelTarget = null;
        $('comparatorDelModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        let url = '', body = {};
        if (t.type === 'gate'){ url = '/comparatorgatedel'; body = { index: t.gi }; }
        else if (t.type === 'input'){ url = '/comparatorinputdel'; body = { gateIndex: t.gi, inputIndex: t.ii }; }
        else { url = '/comparatoroutputdel'; body = { gateIndex: t.gi, actionIndex: t.ai }; }
        fetchJson(url, { method: 'POST', body: JSON.stringify(body) }).then(d => {
            if (d.success){ toast('已删除'); loadComparatorConfig(); }
            else toast(d.error || '删除失败');
        });
    }

    )HTML";

#endif
