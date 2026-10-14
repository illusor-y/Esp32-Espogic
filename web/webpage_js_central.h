// webpage_js_central.h - 从 webpage.h 拆分而来
#ifndef SMARTGLASS_WEBPAGE_JS_CENTRAL_H
#define SMARTGLASS_WEBPAGE_JS_CENTRAL_H

static const char PAGE_JS_CENTRAL[] PROGMEM = R"HTML(// ===== 中心面板（只读监控 + 触发归因） =====
    let centralPolling = false;
    let centralEventsData = [];
    let centralPinsSnap = null;
    let centralEventsSnap = null;
    let centralPwmSnap = null;

    function openCentralPanel(){
        $('centralModal').classList.add('show');
        document.body.classList.add('modal-open');
        if (!centralPolling){
            centralPolling = true;
            refreshCentral();
        }
    }
    function closeCentralPanel(){
        $('centralModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        centralPolling = false;
    }

    function refreshCentral(){
        fetchJson('/centralstatus').then(d => {
            if (!centralPolling) return;
            centralEventsData = d.events || [];
            renderCentral(d);
            setTimeout(refreshCentral, 1000);
        }).catch(() => { centralPolling = false; });
    }

    function centralLvlText(level){
        return level < 0 ? '—' : (level ? '高电平' : '低电平');
    }
    function centralLvlCls(level){
        return level < 0 ? 'lvl-low' : (level ? 'lvl-high' : 'lvl-low');
    }
    function centralEventHtml(e){
        const fromTxt = e.from ? '高' : '低';
        const toTxt = e.to ? '高' : '低';
        const toCls = e.to ? 'lvl-high' : 'lvl-low';
        return '<div class="central-event" onclick="openCentralDetail(' + e.pin + ')">' +
            '<span class="cp-name" style="flex:none;">GPIO ' + e.pin + '</span>' +
            '<span class="' + centralLvlCls(e.from) + '" style="font-size:12px;flex:none;">' + fromTxt + '</span>' +
            '<span class="central-arrow">→</span>' +
            '<span class="' + toCls + '" style="font-size:12px;flex:none;">' + toTxt + '</span>' +
            '<span class="badge" style="background:rgba(79,209,197,.1);border-color:rgba(79,209,197,.32);color:#4fd1c5;">' + esc(e.reason) + '</span>' +
            '<span class="cd-time">' + esc(e.clock) + '</span>' +
        '</div>';
    }

    function renderCentral(d){
        const pins = d.pins || [], events = d.events || [], pwm = d.pwmRunning || [];

        // 监控区：所有配置中的 IO 引脚（仅显示内容变化时重建，避免悬停闪烁）
        const pinsKey = JSON.stringify(pins.map(g => [g.pin, g.level, g.duty != null ? g.duty.toFixed(1) : null, g.driver, g.latch, g.manual, g.flash]));
        if (pinsKey !== centralPinsSnap){
            centralPinsSnap = pinsKey;
            const pinsEl = $('centralPins');
            if (!pins.length){
                pinsEl.innerHTML = '<div class="central-empty" style="grid-column:1/-1;">暂无配置的 IO 引脚</div>';
            } else {
                pinsEl.innerHTML = pins.map(g => {
                    const name = 'GPIO ' + g.pin;
                    const val = g.pwm ? (g.duty != null ? g.duty.toFixed(1) + '%' : 'PWM') : centralLvlText(g.level);
                    const valCls = g.pwm ? 'lvl-high' : centralLvlCls(g.level);
                    const driverTag = '<span class="badge" style="background:rgba(79,209,197,.1);border-color:rgba(79,209,197,.32);color:#4fd1c5;">' + esc(g.driver) + '</span>';
                    const latchTag = g.latch ? '<span class="badge" style="background:rgba(255,196,107,.1);border-color:rgba(255,196,107,.32);color:#ffc46b;">锁存</span>' : '';
                    const manualTag = g.manual ? '<span class="badge">手动</span>' : '';
                    const flashTag = g.flash ? '<span class="badge-danger">PSRAM</span>' : '';
                    return '<div class="central-pin" onclick="openCentralDetail(' + g.pin + ')">' +
                        '<span class="cp-name">' + name + '</span>' +
                        '<span class="' + valCls + '" style="font-size:12px;flex:none;">' + val + '</span>' +
                        driverTag + flashTag + latchTag + manualTag +
                    '</div>';
                }).join('');
            }
        }

        // 行为区：电平反转事件（弹窗内仅显示近 3 条，仅变化时重建）
        const eventsKey = JSON.stringify(events);
        if (eventsKey !== centralEventsSnap){
            centralEventsSnap = eventsKey;
            const evEl = $('centralEvents');
            if (!events.length){
                evEl.innerHTML = '<div class="central-empty">暂无电平反转事件</div>';
            } else {
                evEl.innerHTML = events.slice(0, 3).map(centralEventHtml).join('');
            }
            const expandBtn = $('centralExpandBtn');
            if (expandBtn) expandBtn.style.display = (events.length > 3) ? 'inline-block' : 'none';
        }

        // 行为区：正在运行的 PWM（仅变化时重建）
        const pwmKey = JSON.stringify(pwm.map(p => [p.index, p.pin, p.duty != null ? p.duty.toFixed(1) : 0, p.task]));
        if (pwmKey !== centralPwmSnap){
            centralPwmSnap = pwmKey;
            const pwmEl = $('centralPwm');
            if (!pwm.length){
                pwmEl.innerHTML = '<div class="central-empty">当前无运行中的 PWM</div>';
            } else {
                pwmEl.innerHTML = pwm.map(p => {
                    return '<div class="central-event" onclick="openCentralDetail(' + p.pin + ')">' +
                        '<span class="cp-name" style="flex:none;">CH' + (p.index + 1) + ' (GPIO ' + p.pin + ')</span>' +
                        '<span class="lvl-high" style="font-size:12px;flex:none;">' + (p.duty != null ? p.duty.toFixed(1) : 0) + '%</span>' +
                        '<span class="badge">任务 ' + p.task + '</span>' +
                    '</div>';
                }).join('');
            }
        }
    }

    // ===== 全部反转事件二级弹窗 =====
    function openCentralEvents(){
        renderCentralEventsAll();
        $('centralEventsModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeCentralEvents(){
        $('centralEventsModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function renderCentralEventsAll(){
        const el = $('centralEventsAll');
        if (!centralEventsData.length){
            el.innerHTML = '<div class="central-empty">暂无电平反转事件</div>';
            return;
        }
        el.innerHTML = centralEventsData.map(centralEventHtml).join('');
    }

    // ===== 触发归因详情 =====
    function openCentralDetail(pin){
        $('centralDetailTitle').textContent = 'GPIO ' + pin + ' 触发归因';
        $('centralDetailDesc').textContent = '加载中…';
        $('centralDetailList').innerHTML = '';
        $('centralDetailModal').classList.add('show');
        document.body.classList.add('modal-open');
        fetchJson('/centraldetail?pin=' + pin).then(d => {
            const lvl = d.level < 0 ? 'PWM 输出' : (d.level ? '高电平' : '低电平');
            $('centralDetailDesc').textContent = '当前由「' + (d.driver || '—') + '」驱动，实际电平：' + lvl + '。';
            const items = d.items || [];
            if (!items.length){
                $('centralDetailList').innerHTML = '<div class="central-empty">无归因信息</div>';
                return;
            }
            $('centralDetailList').innerHTML = items.map(it =>
                '<div class="cd-item' + (it.active ? ' cd-active' : '') + '">' +
                    '<span class="cd-module">' + esc(it.module) + '</span>' +
                    '<div class="cd-right">' +
                        '<span class="cd-group">' + esc(it.group) + '</span>' +
                        '<span class="cd-entry">' + esc(it.entry) + '</span>' +
                    '</div>' +
                '</div>'
            ).join('');
        }).catch(() => {});
    }
    function closeCentralDetail(){
        $('centralDetailModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }

    )HTML";

#endif
