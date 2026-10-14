// webpage_js_weather.h - 天气模块（后端抓取 + 前端只读缓存 + 手动省市区）
#ifndef SMARTGLASS_WEBPAGE_JS_WEATHER_H
#define SMARTGLASS_WEBPAGE_JS_WEATHER_H

static const char PAGE_JS_WEATHER[] PROGMEM = R"HTML(// ===== 天气 =====
    let WEATHER = {
        mode: 'auto',            // 'auto' 网络定位 / 'manual' 手动定位
        area: null, areaReady: false,
        prov: null, city: null, dist: null,   // 手动选择的省市区 {code, name}
        location: null,
        updated: '',             // 后端天气最后更新时刻 "HH:MM:SS"（空 = 尚无有效数据）
        text: '', icon: '',      // 当前天气现象文案/图标，供通道卡片显示「大气交互70%(阴 🌧)」
        busy: false,             // 后端正在抓取：期间「手动更新」按钮置灰
        holdUntil: 0,            // 点击后到后端真正置 busy 之间的观察窗上限（有界，防止按钮卡死）
        pendingManual: false,    // 已拨到「手动定位」但尚未点「保存」（切换+写NVS延迟到保存）
        savedMode: 'auto',       // NVS 里实际存的定位模式（判断"拨到手动又拨回"是否真的改过）
        savedSel: '',            // NVS 里存档的手动位置签名（保存按钮的比对基准；空 = 无手动存档）
        // 当日太阳时刻 "HH:MM"（空 = 无数据/当日无此事件）：阳光交互设置弹窗与行摘要显示用
        sunrise: '', sunset: '', dawn: '', dusk: ''
    };

    // 手动位置签名 "省码|市码|区码"：与 NVS 存档比较，判断下拉选择有没有改动
    function weatherSelSign(){
        const c = o => (o && o.code) ? o.code : '';
        return c(WEATHER.prov) + '|' + c(WEATHER.city) + '|' + c(WEATHER.dist);
    }
    // 有没有未保存的改动：还没选省份算"无改动"（按钮保持不可点）
    function weatherManualDirty(){
        if (!WEATHER.prov) return false;
        return weatherSelSign() !== WEATHER.savedSel;
    }
    // 「保存」按钮：只有相对 NVS 存档有改动时才可点（没改动 / 已保存 → 置灰）
    function weatherUpdateSaveBtn(){
        const btn = $('weatherSaveBtn');
        if (!btn) return;
        const manual = (WEATHER.mode === 'manual');
        const dirty  = weatherManualDirty();
        btn.disabled = !manual || !dirty;
        btn.textContent = (manual && !dirty && WEATHER.savedSel) ? '已保存' : '保存';
    }

    // 天气文案/图标由后端按 WMO 天气码统一返回（st.text / st.icon），前端不再维护码表

    // 带超时的 fetch（外部 API / 省市区数据），避免无外网时长时间挂起
    function weatherFetch(url, ms){
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), ms || 8000);
        return fetch(url, { signal: ctrl.signal }).finally(() => clearTimeout(t));
    }

    // ===== 省市区数据（懒加载，用于手动定位）=====
    function weatherLoadArea(){
        if (WEATHER.areaReady) return Promise.resolve();
        return weatherFetch('https://cdn.jsdelivr.net/npm/china-area-data@5.0.1/data.json')
            .then(r => r.json())
            .then(d => {
                WEATHER.area = d; WEATHER.areaReady = true;
                weatherRenderProv(); weatherRestoreSelects();
            })
            .catch(() => {});
    }

    // ===== 自定义下拉（省/市/区）：美化 + 开合动画 + 滚动条 =====
    function wsBtn(k){ return document.querySelector('.w-select[data-ws="' + k + '"] .w-select-btn'); }
    function wsPanel(k){ return document.querySelector('.w-select[data-ws="' + k + '"] .w-select-panel'); }
    function wsVal(k){ return document.querySelector('.w-select[data-ws="' + k + '"] .w-select-val'); }

    function wsCloseAll(except){
        document.querySelectorAll('.w-select.open').forEach(r => {
            if (except == null || r.getAttribute('data-ws') !== except) r.classList.remove('open');
        });
    }

    // ===== 直辖市处理 =====
    // 国标数据里直辖市的"市级"叫「市辖区」（重庆另有一枝「县」），那不是地名。
    // 城市一级统一显示/保存为省名（即"北京市-北京市"）；两枝的区县合并进第三级"选择区县"。
    const WEATHER_MUNI = { '110000': 1, '120000': 1, '310000': 1, '500000': 1 };
    function weatherMuniProv(provCode){
        return (WEATHER.area && WEATHER.area['86'] && WEATHER.area['86'][provCode]) || '';
    }
    // 存进 NVS / 交给后端地理编码的名字：直辖市一律取省名
    function weatherCityName(provCode, raw){
        if (!WEATHER_MUNI[provCode]) return raw;
        return weatherMuniProv(provCode) || raw;
    }
    // 下拉里显示的标签：直辖市显示省名
    function weatherCityLabel(provCode, raw){
        if (!WEATHER_MUNI[provCode]) return raw;
        return weatherMuniProv(provCode) || raw;
    }
    // 城市下拉的数据源：直辖市把国标的「市辖区/县」两枝收成一项 —— 省名。
    // 码沿用「市辖区」那一枝（110100 这类）：旧存档存的就是它，保持一致才不会一开弹窗
    // 就被判成"有改动"而点亮保存按钮。
    function weatherCityMap(provCode){
        const a = (WEATHER.area || {})[provCode] || {};
        if (!WEATHER_MUNI[provCode]) return a;
        const codes = Object.keys(a);
        if (!codes.length) return a;
        const muniCode = codes.find(c => a[c] === '市辖区') || codes[0];
        const one = {};
        one[muniCode] = weatherMuniProv(provCode) || a[muniCode];
        return one;
    }
    // 区县下拉的数据源：直辖市把省下各枝合并（重庆 = 市辖区 + 县）。
    // 第三级本来就叫"选择区县"，两枝都该在这里；国标码跨枝不重复，直接合并安全。
    function weatherDistMap(provCode, cityCode){
        if (!WEATHER_MUNI[provCode]) return (WEATHER.area || {})[cityCode] || {};
        const a = (WEATHER.area || {})[provCode] || {};
        const merged = {};
        Object.keys(a).forEach(c => Object.assign(merged, (WEATHER.area || {})[c] || {}));
        return merged;
    }
    // 区县名反查：直辖市要跨枝找（城口县挂在 500200 枝下，而 cityCode 是 500100）
    function weatherDistName(provCode, cityCode, regionCode){
        if (!weatherValidCode(regionCode)) return '';
        if (WEATHER_MUNI[provCode]){
            const a = (WEATHER.area || {})[provCode] || {};
            for (const c of Object.keys(a)){
                const n = ((WEATHER.area || {})[c] || {})[regionCode];
                if (n) return n;
            }
            return '';
        }
        return ((WEATHER.area || {})[cityCode] || {})[regionCode] || '';
    }

    // 填充某下拉的选项并重置为占位符（未选中）
    function weatherFill(key, obj, ph){
        const panel = wsPanel(key), btn = wsBtn(key), val = wsVal(key);
        if (!panel) return;
        let html = ph ? ('<button type="button" class="w-opt" data-code="">' + esc(ph) + '</button>') : '';
        const keys = obj ? Object.keys(obj) : [];
        keys.forEach(k => { html += '<button type="button" class="w-opt" data-code="' + k + '">' + esc(obj[k]) + '</button>'; });
        panel.innerHTML = html;
        if (btn) btn.dataset.val = '';
        if (val) val.textContent = ph;
    }

    // 设置某下拉的选中值 + 显示名 + 高亮
    function wsSet(key, code, text){
        const btn = wsBtn(key), val = wsVal(key), panel = wsPanel(key);
        if (btn) btn.dataset.val = code || '';
        if (val) val.textContent = text;
        if (panel){
            panel.querySelectorAll('.w-opt').forEach(o => {
                o.classList.toggle('selected', !!code && o.getAttribute('data-code') === String(code));
            });
        }
    }

    function weatherRenderProv(){
        if (!WEATHER.area) return;
        weatherFill('prov', WEATHER.area['86'] || {}, '选择省份');
    }

    function weatherProvChange(code){
        WEATHER.prov = code ? { code: code, name: WEATHER.area['86'][code] } : null;
        WEATHER.city = null; WEATHER.dist = null;
        wsSet('prov', code || '', code ? WEATHER.area['86'][code] : '选择省份');
        weatherFill('city', code ? weatherCityMap(code) : {}, '选择城市');
        weatherFill('dist', {}, '选择区县');
    }

    function weatherCityChange(code){
        const raw = code ? (WEATHER.area[WEATHER.prov.code] || {})[code] : '';
        WEATHER.city = code ? { code: code, name: weatherCityName(WEATHER.prov.code, raw) } : null;
        WEATHER.dist = null;
        wsSet('city', code || '', code ? weatherCityLabel(WEATHER.prov.code, raw) : '选择城市');
        weatherFill('dist', code ? weatherDistMap(WEATHER.prov.code, code) : {}, '选择区县');
    }

    function weatherDistChange(code){
        // 直辖市要用跨枝反查：城口县挂在 500200 枝下，而 cityCode 是 500100
        const n = code ? weatherDistName(WEATHER.prov.code, WEATHER.city.code, code) : '';
        WEATHER.dist = code ? { code: code, name: n } : null;
        wsSet('dist', code || '', code ? n : '选择区县');
    }

    function wsPick(key, code){
        if (key === 'prov') weatherProvChange(code);
        else if (key === 'city') weatherCityChange(code);
        else if (key === 'dist') weatherDistChange(code);
        // 时区弹窗也复用这套自定义下拉（实现见 webpage_js_core.h 的 tzSetHour/tzSetMin）
        else if (key === 'tzHour') tzSetHour(code);
        else if (key === 'tzMin')  tzSetMin(code);
        // 省市区选完立刻刷新「保存」按钮：只有相对存档有改动时才可点
        if (key === 'prov' || key === 'city' || key === 'dist') weatherUpdateSaveBtn();
    }

    // 全局点击（捕获阶段，先于 modal 的 stopPropagation）：开合自定义下拉
    document.addEventListener('click', function(e){
        const t = e.target;
        if (!t || !t.closest) return;
        const btn = t.closest('.w-select-btn');
        if (btn){
            const root = btn.closest('.w-select');
            if (root){
                if (root.classList.contains('open')) root.classList.remove('open');
                else { wsCloseAll(); root.classList.add('open'); }
            }
            return;
        }
        const opt = t.closest('.w-opt');
        if (opt){
            const root = opt.closest('.w-select');
            if (root) wsPick(root.getAttribute('data-ws'), opt.getAttribute('data-code') || '');
            wsCloseAll();
            return;
        }
        if (!t.closest('.w-select')) wsCloseAll();
    }, true);

    // 点击「保存」按钮：真正切换为手动模式并写 NVS，后端重新抓取
    // 只允许"有改动"时点（按钮本身已按 weatherManualDirty() 置灰，这里再兜一道）
    async function weatherManualSave(){
        if (!WEATHER.prov){ toast('请先选择省市区'); return; }
        if (!weatherManualDirty()){ toast('位置未变化'); return; }
        WEATHER.pendingManual = false;
        try { await weatherSaveManual(); } catch(e){}
        // 存档已与当前选择一致：记下签名，保存按钮随之置灰
        WEATHER.savedMode = 'manual';
        WEATHER.savedSel  = weatherSelSign();
        weatherUpdateSaveBtn();
        toast('已保存手动位置，正在定位…');
        weatherPoll();
    }

    // 保存手动位置到 NVS（返回 Promise，便于调用方等待保存完成后再轮询）
    function weatherSaveManual(){
        const body = {
            mode: 'manual',
            provCode: WEATHER.prov ? WEATHER.prov.code : '',
            cityCode: WEATHER.city ? WEATHER.city.code : '',
            distCode: WEATHER.dist ? WEATHER.dist.code : '',
            prov: WEATHER.prov ? WEATHER.prov.name : '',
            city: WEATHER.city ? WEATHER.city.name : '',
            dist: WEATHER.dist ? WEATHER.dist.name : ''
        };
        return fetchJson('/weathersave', { method: 'POST', body: JSON.stringify(body) }).catch(() => {});
    }

    // 保存网络模式到 NVS（同时清除手动记录）
    function weatherSaveAuto(){
        return fetchJson('/weathersave', { method: 'POST', body: JSON.stringify({ mode: 'auto' }) }).catch(() => {});
    }

    // 自动模式下点「手动更新」按钮：重新触发后端定位并轮询
    function weatherReAuto(){
        if (weatherRefreshLocked()){ weatherUpdateRefreshBtn(); return; }
        // 点击瞬间就置灰：busy 要等后端真正进入抓取才有，只靠 busy 判定会在这段空窗里
        // 被下一轮轮询解禁（用户会以为没生效、还能连点）。
        WEATHER.holdUntil = Date.now() + 5000;
        WEATHER.mode = 'auto';
        weatherRenderMode();
        weatherSaveAuto();
        toast('正在定位…');
        weatherPoll();
    }

    // 「手动更新」按钮是否处于不可用状态：抓取中，或处于点击后的观察窗内
    function weatherRefreshLocked(){
        return WEATHER.busy || (WEATHER.holdUntil > 0 && Date.now() < WEATHER.holdUntil);
    }

    // 仅在抓取进行中置灰（节流已取消，不再有 10 分钟倒计时）
    function weatherUpdateRefreshBtn(){
        const btn = $('weatherRefreshBtn');
        if (!btn) return;
        const locked = weatherRefreshLocked();
        btn.disabled = locked;
        btn.textContent = locked ? '更新中…' : '手动更新';
    }

    function weatherSetTxt(id, v){ const el = $(id); if (el) el.textContent = v; }

    // 温度显示一位小数（后端返回 float，前端固定 1 位，如 26.5°）
    function weatherFmt1(v){ return (v == null || isNaN(v)) ? '--' : Number(v).toFixed(1); }

    function weatherValidCode(c){ return !!c && String(c) !== '0' && String(c) !== '00'; }

    // 按国标行政区划码反查中文名（省+市+区）
    function weatherNameByCode(proCode, cityCode, regionCode){
        const a = WEATHER.area;
        if (!a || !a['86']) return '';
        const prov = a['86'][proCode] || '';
        if (!prov) return '';
        // 直辖市：市级是「市辖区」/「县」，跳过市级直接拼 省 + 区（如 北京市东城区、重庆市城口县）。
        // 区名走 weatherDistName 跨枝查：城口县挂在 500200 枝下，而 cityCode 是 500100。
        // 旧判断 cityCode===proCode 在本数据里永不成立 —— 直辖市的市级码是 110100 之类，不是省码。
        if (WEATHER_MUNI[proCode]) return prov + weatherDistName(proCode, cityCode, regionCode);
        if (!weatherValidCode(cityCode) || cityCode === proCode){
            return prov + (weatherValidCode(regionCode) && a[proCode] ? (a[proCode][regionCode] || '') : '');
        }
        const city = (a[proCode] && a[proCode][cityCode]) ? a[proCode][cityCode] : '';
        if (!city) return prov;
        const dist = (weatherValidCode(regionCode) && a[cityCode]) ? (a[cityCode][regionCode] || '') : '';
        return prov + city + dist;
    }

    // 渲染定位文本：显示「IP/城市」；无 IP 时仅显示城市名；无城市名时回落坐标
    function weatherRenderLocation(){
        const loc = WEATHER.location;
        if (!loc) return;
        const render = (city) => {
            const ip = loc.ip || '';
            const c = city || '';
            weatherSetTxt('weatherLocText', (ip && c) ? (ip + '/' + c) : (ip || c || ''));
        };
        if (loc.name){
            render(loc.name);
        } else {
            const fallback = (loc.lat != null) ? (loc.lat + ',' + loc.lon) : '';
            if (weatherValidCode(loc.proCode)){
                weatherLoadArea().then(() => {
                    render(weatherNameByCode(loc.proCode, loc.cityCode, loc.regionCode) || fallback);
                }).catch(() => render(fallback));
            } else {
                render(fallback);
            }
        }
        weatherRenderLocDebug();
    }

    // 定位信息行：左侧「定位源」，右侧「天气最后更新时间」；两者皆空则整行隐藏
    function weatherRenderLocDebug(){
        const dbg = $('weatherLocDebug');
        if (!dbg) return;
        const source = (WEATHER.location && WEATHER.location.source) || '';
        const updated = WEATHER.updated || '';
        const srcEl = $('weatherLocSource');
        const updEl = $('weatherUpdatedAt');
        if (srcEl) srcEl.textContent = source ? ('定位源：' + source) : '';
        if (updEl) updEl.textContent = updated ? ('最后更新于：' + updated) : '';
        dbg.style.display = (source || updated) ? '' : 'none';
    }

    // 渲染后端返回的天气状态（位置 + 天气缓存）
    function weatherRenderStatus(st){
        if (!st) return;
        // 待保存的手动切换（pendingManual）期间不回拉后端 mode，避免开关被弹回导致第一次切换失败
        if (!WEATHER.pendingManual){
            WEATHER.mode = (st.mode === 'manual') ? 'manual' : 'auto';
            weatherRenderMode();
            const sw = $('weatherModeSwitch'); if (sw) sw.checked = (WEATHER.mode === 'manual');
        }

        WEATHER.updated = st.updated || '';

        if (st.located){
            WEATHER.location = { lat: st.lat, lon: st.lon, source: st.source || '',
                                 name: st.city || st.name || '', ip: st.ip || '',
                                 proCode: st.proCode, cityCode: st.cityCode, regionCode: st.regionCode };
            weatherRenderLocation();
        }
        weatherRenderLocDebug();

        if (st.weatherOk){
            const text = st.text || '未知';
            const icon = st.icon || '🌡️';
            WEATHER.text = text;
            WEATHER.icon = icon;
            weatherUpdateFab(weatherFmt1(st.temperature), icon);
            weatherSetTxt('weatherNowIcon', icon);
            weatherSetTxt('weatherNowTemp', weatherFmt1(st.temperature) + '°');
            weatherSetTxt('weatherNowText', text);
            weatherSetTxt('weatherFeels', weatherFmt1(st.feels) + '°');
            weatherSetTxt('weatherHumidity', weatherFmt1(st.humidity) + '%');
            weatherSetTxt('weatherWind', weatherFmt1(st.wind) + ' m/s');
            weatherSetTxt('weatherToday', weatherFmt1(st.tmax) + '° / ' + weatherFmt1(st.tmin) + '°');
        } else {
            weatherUpdateFab(null, '🌡️');
        }
        // 当日太阳时刻：日出/日落来自 Open-Meteo，晨光/昏光为后端本地算法结果。
        // 后端返回空串 = 暂无数据或当日无此事件（极昼/极夜），回落成占位符。
        // 同时缓存进 WEATHER：阳光交互设置弹窗要显示"今天能不能用"以及用哪两个时刻。
        WEATHER.sunrise = st.sunrise || '';
        WEATHER.sunset  = st.sunset  || '';
        WEATHER.dawn    = st.dawn    || '';
        WEATHER.dusk    = st.dusk    || '';
        weatherSetTxt('weatherSunrise', st.sunrise || '--:--');
        weatherSetTxt('weatherSunset',  st.sunset  || '--:--');
        weatherSetTxt('weatherDawn',    st.dawn    || '--:--');
        weatherSetTxt('weatherDusk',    st.dusk    || '--:--');
        // 阳光交互：太阳时刻刚到/刚失效时刷新弹窗里的摘要（数据 30 秒轮询一次）
        refreshSunTexts();
        // 抓取进行中 → 「手动更新」按钮置灰；后端一旦确认 busy 就撤掉观察窗，交给 busy 接管
        WEATHER.busy = !!st.busy;
        if (WEATHER.busy) WEATHER.holdUntil = 0;
        weatherUpdateRefreshBtn();
    }

    // 轮询后端天气状态（抓取中或处于点击观察窗内时继续轮询）
    async function weatherPoll(){
        try {
            const st = await fetchJson('/weatherstatus');
            weatherRenderStatus(st);
            if (st && (st.busy || weatherRefreshLocked())){ setTimeout(weatherPoll, 2000); }
        } catch(e){
            setTimeout(weatherPoll, 3000);
        }
    }

    // ===== 模式切换（开关）=====
    async function weatherModeToggle(sw){
        const manual = !!sw.checked;
        if (manual){
            // 拨到手动：仅展示选择界面 + 保存按钮，等点「保存」再真正切换 + 写 NVS
            WEATHER.mode = 'manual';
            WEATHER.pendingManual = true;
            transitionModalHeight($('weatherModal'), () => weatherRenderMode());
            weatherLoadArea();
        } else {
            // 拨回自动：只有 NVS 里真的存着「手动」时才需要切换 —— 那样才有东西可改、才该重新定位。
            // 若只是拨过去看了一眼、没点保存（NVS 仍是 auto），就什么都不写、也不触发抓取，
            // 免得平白把正在显示的数据重新定位一遍。
            WEATHER.pendingManual = false;
            const real = (WEATHER.savedMode === 'manual');
            WEATHER.mode = 'auto';
            transitionModalHeight($('weatherModal'), () => weatherRenderMode());
            if (!real){
                toast('已切回网络定位');
                return;
            }
            WEATHER.holdUntil = Date.now() + 5000;   // 这次确实会触发一次抓取，按钮一并置灰
            try { await weatherSaveAuto(); } catch(e){}
            WEATHER.savedMode = 'auto';
            WEATHER.savedSel  = '';
            toast('已切换为网络定位…');
            weatherPoll();
        }
    }

    function weatherRenderMode(){
        const manual = (WEATHER.mode === 'manual');
        const selects = $('weatherSelects');
        const autoRow = $('weatherAutoRow');
        const saveBtn = $('weatherSaveBtn');
        if (selects) selects.style.display = manual ? '' : 'none';
        // 「手动更新」只在网络定位下有意义（手动模式靠「保存」触发定位），所以整行隐藏。
        // 以前只隐按钮、留着整行，是因为「接口转发」原本也在这行；它已移到上方开关行最右侧，
        // 这行现在只剩一个按钮，留空行只会多出一段空白。
        if (autoRow) autoRow.style.display = manual ? 'none' : '';
        if (saveBtn) saveBtn.style.display = manual ? '' : 'none';
        weatherUpdateSaveBtn();   // 与模式一起刷新（没改动时置灰）
    }

    function weatherRestoreSelects(){
        if (!WEATHER.area || !WEATHER.areaReady) return;
        if (WEATHER.prov){
            wsSet('prov', WEATHER.prov.code, WEATHER.area['86'][WEATHER.prov.code] || WEATHER.prov.name || '');
            weatherFill('city', weatherCityMap(WEATHER.prov.code), '选择城市');
        }
        if (WEATHER.city){
            const raw = (WEATHER.area[WEATHER.prov.code] || {})[WEATHER.city.code] || WEATHER.city.name || '';
            // 内存里的名字也一并修正：NVS 里可能是旧存档的「市辖区」，
            // 这样用户不碰下拉直接点保存时，回写的是「北京市」而不是「市辖区」。
            WEATHER.city.name = weatherCityName(WEATHER.prov.code, raw);
            wsSet('city', WEATHER.city.code, weatherCityLabel(WEATHER.prov.code, raw));
            // 区县列表要合并直辖市的各枝（重庆：市辖区 + 县）。注意不能直接用 cityCode 取数据源：
            // 旧存档里可能是「县」那一枝（500200），那样会漏掉市辖区。
            weatherFill('dist', weatherDistMap(WEATHER.prov.code, WEATHER.city.code), '选择区县');
        }
        if (WEATHER.dist){
            // 跨枝反查：旧存档的城口县（500229）挂在「县」枝下，直接查 500100 会取不到名字
            const n = weatherDistName(WEATHER.prov.code, WEATHER.city.code, WEATHER.dist.code) || WEATHER.dist.name || '';
            wsSet('dist', WEATHER.dist.code, n);
        }
        weatherUpdateSaveBtn();   // 恢复出来的选择若与存档一致，保存按钮应保持置灰
    }

    // ===== UI =====
    function weatherUpdateFab(temp, icon){
        const ic = $('weatherIcon');
        const tp = $('weatherTemp');
        if (ic) ic.textContent = icon || '🌡️';
        if (tp) tp.textContent = (temp == null) ? '--°' : (temp + '°');
    }

    function openWeather(){
        if (isApMode){
            toast('AP模式下不支持此功能');
            return;
        }
        $('weatherModal').classList.add('show');
        document.body.classList.add('modal-open');
        weatherRenderMode();
        const sw = $('weatherModeSwitch');
        if (sw) sw.checked = (WEATHER.mode === 'manual');
        if (WEATHER.mode === 'manual'){
            weatherLoadArea();
        }
        weatherRenderLocation();
        weatherUpdateRefreshBtn();
    }
    function closeWeather(){
        $('weatherModal').classList.remove('show');
        document.body.classList.remove('modal-open');
        // 拨到手动但未点「保存」就关闭：回到 NVS 里实际存的模式（没写过盘，必须保持一致）。
        // 注意不是无条件回 auto —— 存档本来就是手动时，回 auto 反而会让界面与设备状态不一致。
        if (WEATHER.pendingManual){
            WEATHER.pendingManual = false;
            WEATHER.mode = WEATHER.savedMode;
            weatherRenderMode();
            const sw = $('weatherModeSwitch'); if (sw) sw.checked = (WEATHER.mode === 'manual');
        }
    }

    // ===== 接口转发二级弹窗 =====
    // 地址由用户自填（Cloudflare Worker 等），直连都失败时才用；留空 = 关闭该级。
    let relayTestTimer = null;

    function relayTestSetResult(text, color){
        const el = $('relayTestResult');
        if (!el) return;
        el.textContent = text || '';
        el.style.color = color || '';
    }

    function openRelay(){
        const inp = $('relayUrl');
        if (inp) inp.value = '';
        relayTestStop();                 // 清掉上一轮可能残留的轮询
        relayTestSetResult('', '');
        // 打开时先拉当前值，避免用户在没有回显的情况下误存一个空串把配置清掉
        fetchJson('/relayget').then(d => {
            if (inp) inp.value = (d && d.url) ? d.url : '';
        }).catch(() => {});
        $('relayModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeRelay(){
        relayTestStop();                 // 关窗就停轮询，别让定时器留在后台空转
        $('relayModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function relayTestStop(){
        if (relayTestTimer){ clearInterval(relayTestTimer); relayTestTimer = null; }
        const btn = $('relayTestBtn');
        if (btn) btn.disabled = false;
    }

    // 「测试联通」：先用当前输入框里的地址触发一次真实请求，再轮询结果。
    // 请求本身最多 8 秒，但若设备此刻正在抓取天气（最多三轮 24 秒），测试要排在它后面，
    // 所以这里留 45 秒窗口兜底 —— 窗口太小会把"排队中"误报成"超时"。
    async function relayTest(){
        const inp = $('relayUrl');
        const url = inp ? inp.value.trim() : '';
        if (!url){ toast('请先填写地址'); return; }
        relayTestStop();
        const btn = $('relayTestBtn');
        if (btn) btn.disabled = true;
        relayTestSetResult('正在测试…（若设备正在抓取天气，需等它跑完）', '');
        try {
            const r = await fetchJson('/relaytest', { method: 'POST', body: JSON.stringify({ url }) });
            if (!r || r.success === false){
                relayTestStop();
                relayTestSetResult((r && r.msg) ? r.msg : '测试启动失败', 'var(--warn)');
                return;
            }
        } catch(e){ relayTestStop(); return; }

        let waited = 0;
        relayTestTimer = setInterval(() => {
            waited += 400;
            // 轮询用裸 fetch 而不是 fetchJson：后者失败会弹 toast，400ms 一次会刷屏
            fetch('/relayteststatus').then(r => r.json()).then(d => {
                if (!d || !d.done){
                    if (waited > 45000){ relayTestStop(); relayTestSetResult('测试超时：设备无响应', 'var(--danger)'); }
                    return;
                }
                relayTestStop();
                const ms = d.ms ? ('，耗时 ' + d.ms + ' ms') : '';
                if (d.ok){
                    const t = (d.temp != null && d.temp > -900) ? ('，返回温度 ' + Number(d.temp).toFixed(1) + '°C') : '';
                    relayTestSetResult('接口可用' + ms + t, 'var(--text-muted)');
                } else {
                    relayTestSetResult((d.msg || '测试失败') + ms, 'var(--danger)');
                }
            }).catch(() => {});
        }, 400);
    }

    async function relaySave(){
        const inp = $('relayUrl');
        const url = inp ? inp.value.trim() : '';
        try {
            const r = await fetchJson('/relaysave', { method: 'POST', body: JSON.stringify({ url }) });
            if (r && r.success === false){ toast(r.msg || '保存失败'); return; }
            toast(url ? '已保存接口转发' : '已关闭接口转发');
            closeRelay();
        } catch(e){}
    }

    // ===== 交互二级弹窗（左：气象交互 = 天气系数；右：阳光交互 = 太阳时刻曲线）=====
    function openWeatherInteract(){
        renderWeatherInteract();
        wxFadeLoad();
        $('weatherInteractModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeWeatherInteract(){
        closeSunSetup();   // 三级弹窗可能还开着（从「设置」进来的），一并收起
        $('weatherInteractModal').classList.remove('show');
        document.body.classList.remove('modal-open');
    }
    function renderWeatherInteract(){
        renderWiNow();
        renderWeatherRows();
        renderSunRows();
    }

    // 顶部公共条右侧的"当前"：当前天气现象 + 生效档位（两栏共用，故放公共条而不是某一栏）
    function renderWiNow(){
        const el = $('wiNow');
        if (!el) return;
        const fac = (CONFIG && typeof CONFIG.weatherFactor === 'number') ? CONFIG.weatherFactor : 100;
        el.textContent = WEATHER.text
            ? (WEATHER.text + (WEATHER.icon || '') + ' · 系数 ' + fac + '%')
            : '未获取天气 · 系数 ' + fac + '%';
    }

    // 天气系数过渡时长（分钟）：0 = 立即切换。后端存秒，换算只在这一层做。
    // 上限 30 分钟（= 天气刷新间隔的一半），由后端 WX_FADE_MAX_SEC 换算后下发。
    // 用裸 fetch 读取：弹窗打开时若请求失败不该弹 toast（与 loadChipRes 同理）。
    function wxFadeLoad(){
        fetch('/wxfadeget').then(r => r.json()).then(d => {
            const el = $('wxFadeMin');
            if (!el || !d) return;
            if (d.maxMin) el.max = d.maxMin;
            if (d.sec != null) el.value = Math.round(d.sec / 60);
        }).catch(() => {});
    }
    function wxFadeSave(){
        const el = $('wxFadeMin');
        if (!el) return;
        const maxMin = Number(el.max) || 50;
        const min = val('wxFadeMin', 25, 0, maxMin);
        el.value = min;   // 夹紧后回写，避免输入框里留着越界值
        fetchJson('/wxfadesave', { method: 'POST', body: JSON.stringify({ sec: min * 60 }) })
            .then(d => {
                if (d && d.success){
                    toast(min > 0 ? ('天气过渡已设为 ' + min + ' 分钟') : '天气过渡已关闭（立即切换）');
                } else {
                    toast((d && d.error) || '保存失败');
                }
            })
            .catch(() => {});
    }

    // 左栏摘要文字（与 renderWeatherRows / 开关切换后共用同一份逻辑）
    function wiSumText(ch){
        if (!ch.weatherInteract) return '未开启';
        const fac = (CONFIG && typeof CONFIG.weatherFactor === 'number') ? CONFIG.weatherFactor : 100;
        const duty = (typeof ch.currentDuty === 'number') ? ch.currentDuty : 0;
        return duty.toFixed(1) + '% × ' + fac + '% = ' + (duty * fac / 100).toFixed(1) + '%';
    }
    // 切换开关后就地刷新摘要（不重渲染整个列表，避免打断用户操作）
    function updateWiSum(i){
        const el = $('wi_sum_' + i);
        const ch = (CONFIG && CONFIG.channels) ? CONFIG.channels[i] : null;
        if (el && ch) el.textContent = wiSumText(ch);
    }

    // 左栏：气象交互（每通道一个开关，占空比 × 天气系数）
    function renderWeatherRows(){
        const box = $('weatherInteractList');
        if (!box) return;
        const chs = (CONFIG && CONFIG.channels) ? CONFIG.channels : [];
        const pins = (CONFIG && CONFIG.pwmPins) ? CONFIG.pwmPins : [];
        if (!chs.length){
            box.innerHTML = '<div class="wi-empty">暂无 PWM 通道，请先在「PWM 信号」页添加通道。</div>';
            return;
        }
        box.innerHTML = chs.map((ch, i) => {
            const pin = (typeof pins[i] === 'number') ? pins[i] : '?';
            return '<div class="wi-row">' +
                '<span class="wi-pin">GPIO ' + pin + '</span>' +
                '<span class="wi-sum" id="wi_sum_' + i + '">' + esc(wiSumText(ch)) + '</span>' +
                '<label class="switch-row" style="cursor:pointer;">' +
                    '<input type="checkbox" class="switch" id="wi_sw_' + i + '" ' + (ch.weatherInteract ? 'checked' : '') + ' onchange="weatherInteractToggle(' + i + ', this)">' +
                '</label>' +
            '</div>';
        }).join('');
    }

    // ===== 右栏：阳光交互 =====
    // 开关打开 → 先进设置弹窗（基准 + 峰值），确认后才真正开启；关闭 → 直接关（参数保留）
    let sunEditing = -1;        // 正在设置的通道；-1 = 无
    let sunPendingMode = 0;     // 弹窗内暂存的基准：0=日出→日落 1=晨光→昏光

    function sunModeName(m){ return (m === 1) ? '晨光→昏光' : '日出→日落'; }
    // 某基准对应的两端时刻是否都有数据（空 = 后端未抓到，此时通道回退条目）
    function sunPairOk(m){
        return (m === 1) ? (!!WEATHER.dawn && !!WEATHER.dusk)
                         : (!!WEATHER.sunrise && !!WEATHER.sunset);
    }
    function renderSunRows(){
        const box = $('sunInteractList');
        if (!box) return;
        const chs = (CONFIG && CONFIG.channels) ? CONFIG.channels : [];
        const pins = (CONFIG && CONFIG.pwmPins) ? CONFIG.pwmPins : [];
        if (!chs.length){
            box.innerHTML = '<div class="wi-empty">暂无 PWM 通道，请先在「PWM 信号」页添加通道。</div>';
            return;
        }
        box.innerHTML = chs.map((ch, i) => {
            const pin = (typeof pins[i] === 'number') ? pins[i] : '?';
            return '<div class="wi-row">' +
                '<span class="wi-pin">GPIO ' + pin + '</span>' +
                '<span class="wi-sum" id="sun_sum_' + i + '">' + esc(sunSummaryText(ch)) + '</span>' +
                '<button type="button" class="btn tonal wi-set-btn" onclick="openSunSetup(' + i + ')">设置</button>' +
                '<label class="switch-row" style="cursor:pointer;">' +
                    '<input type="checkbox" class="switch" id="sun_sw_' + i + '" ' + (ch.sunEnabled ? 'checked' : '') + ' onchange="sunToggle(' + i + ', this)">' +
                '</label>' +
            '</div>';
        }).join('');
    }
    // 一行摘要（渲染与轮询刷新共用同一份文案逻辑）
    function sunSummaryText(ch){
        const mode = (ch.sunMode === 1) ? 1 : 0;
        const peak = (typeof ch.sunPeakDuty === 'number') ? ch.sunPeakDuty : 100;
        if (!ch.sunEnabled) return '未开启';
        // 无数据 = 该通道此刻走条目逻辑：模式/时刻/峰值全都不生效，一行都不写（写了会误导）
        if (!sunPairOk(mode)) return '（无数据退回条目）';
        const a = (mode === 1) ? WEATHER.dawn : WEATHER.sunrise;
        const b = (mode === 1) ? WEATHER.dusk : WEATHER.sunset;
        return sunModeName(mode) + ' ' + a + '→' + b + ' · ' + peak + '%';
    }
    // 天气状态轮询到达后刷新阳光相关文字（只改文本、不重建 DOM，避免打断正在操作的开关）
    function refreshSunTexts(){
        const chs = (CONFIG && CONFIG.channels) ? CONFIG.channels : [];
        chs.forEach((ch, i) => {
            const el = $('sun_sum_' + i);
            if (el) el.textContent = sunSummaryText(ch);
        });
        const ss = $('sunSetupModal');
        if (ss && ss.classList.contains('show')) renderSunSetup();
    }
    function sunToggle(i, sw){
        const ch = (CONFIG && CONFIG.channels) ? CONFIG.channels[i] : null;
        if (!ch) return;
        if (sw && sw.checked){
            sw.checked = false;   // 回滚：确认设置后才由后端状态点亮（避免"开了但没参数"的中间态）
            openSunSetup(i);
        } else {
            sunCommit(i, false, (ch.sunMode === 1) ? 1 : 0, ch.sunPeakDuty, '已关闭阳光交互');
        }
    }
    function openSunSetup(i){
        const ch = (CONFIG && CONFIG.channels) ? CONFIG.channels[i] : null;
        if (!ch) return;
        sunEditing = i;
        sunPendingMode = (ch.sunMode === 1) ? 1 : 0;
        const pins = (CONFIG && CONFIG.pwmPins) ? CONFIG.pwmPins : [];
        const pin = (typeof pins[i] === 'number') ? pins[i] : '?';
        $('sunSetupPin').textContent = 'GPIO ' + pin + ' · 起点 0% → 中点达到设定峰值 → 终点 0%';
        $('sunPeakInput').value = (typeof ch.sunPeakDuty === 'number') ? ch.sunPeakDuty : 100;
        renderSunSetup();
        $('sunSetupModal').classList.add('show');
        document.body.classList.add('modal-open');
    }
    function closeSunSetup(){
        sunEditing = -1;
        $('sunSetupModal').classList.remove('show');
        // 二级弹窗还开着时不移除 modal-open，否则背景滚动会被意外放开
        const wi = $('weatherInteractModal');
        if (!(wi && wi.classList.contains('show'))) document.body.classList.remove('modal-open');
    }
    function renderSunSetup(){
        $('sunModeBtn0').classList.toggle('active', sunPendingMode === 0);
        $('sunModeBtn1').classList.toggle('active', sunPendingMode === 1);
        const a = (sunPendingMode === 1) ? WEATHER.dawn : WEATHER.sunrise;
        const b = (sunPendingMode === 1) ? WEATHER.dusk : WEATHER.sunset;
        const label = (sunPendingMode === 1) ? '晨光 / 昏光' : '日出 / 日落';
        $('sunSetupTimes').innerHTML = (a && b)
            ? ('今日 ' + label + '：<b>' + a + '</b> → <b>' + b + '</b>')
            : ('无数据退回条目');
    }
    function sunSetMode(m){
        sunPendingMode = (m === 1) ? 1 : 0;
        renderSunSetup();
    }
    function sunSetupConfirm(){
        if (sunEditing < 0) return;
        const peak = val('sunPeakInput', 100, 0, 100);
        sunCommit(sunEditing, true, sunPendingMode, peak, '已开启阳光交互', () => closeSunSetup());
    }
    // 统一提交（开/关同一路径）：成功后刷新两栏、通道卡片与状态
    function sunCommit(i, enabled, mode, peak, okMsg, done){
        fetchJson('/pwmsun', { method: 'POST', body: JSON.stringify({ channel: i, enabled: enabled, mode: mode, peakDuty: peak }) })
            .then(d => {
                if (!d.success){ toast(d.error || '操作失败'); return; }
                const ch = (CONFIG && CONFIG.channels) ? CONFIG.channels[i] : null;
                if (ch){
                    ch.sunEnabled = !!d.sunEnabled;
                    ch.sunMode = d.sunMode;
                    ch.sunPeakDuty = d.sunPeakDuty;
                }
                toast((d.sunEnabled && d.dataOk === false)
                        ? (okMsg + '（无数据退回条目）')
                        : okMsg);
                if (done) done();
                renderSunRows();
                // 先重渲染通道卡片（条目禁用/淡红标签/公式显示），再刷状态，避免顺序颠倒读到旧 DOM
                loadConfig().then(() => loadStatus());
            })
            .catch(() => {});
    }
    function weatherInteractToggle(i, sw){
        // 目标值以 CONFIG 中的后端状态取反（后端返回后再回写），不依赖原生 checkbox.checked，
        // 与 ioToggle/counterToggle 一致：最终状态一律以接口返回值为准。
        const cur = (CONFIG && CONFIG.channels && CONFIG.channels[i]) ? !!CONFIG.channels[i].weatherInteract : false;
        const on = !cur;
        fetchJson('/pwmweathertoggle', { method: 'POST', body: JSON.stringify({ channel: i, enabled: on }) })
            .then(d => {
                if (d.success){
                    const v = !!d.weatherInteract;
                    if (CONFIG && CONFIG.channels && CONFIG.channels[i]) CONFIG.channels[i].weatherInteract = v;
                    if (sw) sw.checked = v;
                    updateWiSum(i);   // 摘要跟着开关变（"未开启" ↔ "62.0% × 80% = 49.6%"）
                    toast((v ? '已开启' : '已关闭') + '气象交互');
                    loadStatus();
                } else {
                    if (sw) sw.checked = cur;
                    toast(d.error || '操作失败');
                }
            })
            .catch(() => { if (sw) sw.checked = cur; });
    }

    // ===== 启动：读配置（手动名恢复）+ 轮询后端天气状态 =====
    async function weatherInit(){
        try {
            const cfg = await fetchJson('/weatherget');
            // 与后端一致：仅当 NVS 中既有 manual 模式又有手动位置信息时才视为手动，否则回退自动
            const manualOk = (cfg.mode === 'manual') && !!(cfg.provCode || cfg.prov);
            WEATHER.mode = manualOk ? 'manual' : 'auto';
            // 记下 NVS 真值：savedMode 用于"拨过去又拨回来"的判定，
            // savedSel 是保存按钮的比对基准（存档里没有手动位置时留空）。
            WEATHER.savedMode = WEATHER.mode;
            WEATHER.savedSel  = '';
            if (manualOk){
                WEATHER.prov = { code: cfg.provCode || '', name: cfg.prov || '' };
                WEATHER.city = cfg.city ? { code: cfg.cityCode || '', name: cfg.city } : null;
                WEATHER.dist = cfg.dist ? { code: cfg.distCode || '', name: cfg.dist } : null;
                WEATHER.savedSel = weatherSelSign();
                weatherLoadArea();
            }
            weatherRenderMode();
        } catch(e){}
        weatherPoll();
    }
    weatherInit();
    setInterval(weatherUpdateRefreshBtn, 1000);
    // 周期轮询天气状态：后端每小时整点会静默自动抓取并更新「最后更新」时刻，
    // 若前端不主动轮询，已打开的弹窗将停留在旧数据，必须刷新整页才能看到。
    setInterval(weatherPoll, 30000);
    )HTML";

#endif