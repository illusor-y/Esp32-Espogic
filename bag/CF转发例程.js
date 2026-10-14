// ============================================================================
//  Open-Meteo 天气转发 —— Cloudflare Worker 单文件例程
// ============================================================================
//
//  【它是干什么的】
//  给 Open-Meteo 的查询接口做一层反向代理。设备（ESP32 等）改连这个地址，
//  就绕开了"直连 api.open-meteo.com 时那条长距离路径不稳定"的问题。
//
//  【为什么需要】
//  api.open-meteo.com 是德国的裸源站 IP（无 CDN）。国内部分宽带出口到它的路由
//  会间歇性整体黑洞：实测 3 秒内一个 TCP SYN 都回不来；有时连上了却拿不到响应
//  （HTTPClient 的 8 秒"读静默窗口"会被每个数据包重置，所以能一路拖到 41 秒才判超时）。
//  Cloudflare 走 anycast，国内到近端边缘的互联质量明显更好，改成
//     设备 → CF 边缘 → 源站
//  之后，长距离那一截就交给机房线路去跑了。
//
//  【怎么部署】（免费，不需要信用卡）
//  1. 登录 dash.cloudflare.com → 左侧 Workers & Pages → Create → Worker
//  2. 起个名字（如 weather-relay）→ Deploy → 再点 Edit code
//  3. 把本文件全部内容粘进去 → Deploy
//  4. 得到形如下面的地址：
//         https://weather-relay.你的账号.workers.dev
//
//  【自测】
//    curl "https://<你的地址>/api/weather?latitude=39.9042&longitude=116.4074&daily=weather_code&timezone=auto&forecast_days=1"
//  正常应返回一段 JSON；返回 400 说明经纬度没带上。
//
//  【设备端怎么填】
//  打开设备网页 → 天气弹窗 → 「接口转发」→ 填：
//         https://weather-relay.你的账号.workers.dev/api/weather
//  点「测试联通」，看到"接口可用，返回温度 xx°C"就算通，再点保存。
//
//  设备端共有三级降级，本项是最后一级，留空即关闭：
//         ① HTTPS 直连  →  ② 明文 HTTP 直连  →  ③ 本转发
//  只有前两级都失败时才会用到这里，所以不会白占你的额度。
//
//  【可选：加个密钥】
//  本路由默认不校验，谁拿到地址都能用。想只服务自己的设备，就把下面 RELAY_KEY
//  设成随机串，设备端地址改成：
//         https://<你的地址>/api/weather?key=那个随机串
//
//  【给二次分发的人】
//  请自己部署一份，不要共用别人的地址 —— 这既是礼貌，也避免对方一旦改密钥、
//  停服或超额度，把你的设备一起带下水。
//  免费额度 10 万次/天：设备每小时查一次，约够 4000 台。
//
//  【数据来源与署名】
//  天气数据来自 Open-Meteo（CC BY 4.0）。对外发布产品或页面时请保留署名：
//  https://open-meteo.com/
//
// ============================================================================

// ===== 可配置项 =====
const RELAY_KEY  = '';   // 留空 = 不校验；填随机串 = 开启 ?key= 校验
const UPSTREAM   = 'https://api.open-meteo.com/v1/forecast';
const CACHE_TTL  = 600;  // 边缘缓存秒数：同一坐标这段时间内直接命中缓存，不再回源
const TIMEOUT_MS = 8000; // 上游超时：卡住就快速失败，别让设备干等

// 只放行这些查询参数，避免本路由被当成通用 Open-Meteo 代理来刷。
// 注意 key 不在名单里 —— 所以它永远不会被转发给上游。
const ALLOWED_PARAMS = [
  'latitude', 'longitude', 'current', 'daily',
  'timezone', 'forecast_days', 'wind_speed_unit'
];

// 浏览器里直接调试时有用（设备端不需要 CORS，加上只为方便你在网页地址栏试）
const CORS_HEADERS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET,OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type'
};

const jsonResponse = (obj, status) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { ...CORS_HEADERS, 'Content-Type': 'application/json; charset=utf-8' }
  });

export default {
  async fetch(request) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: CORS_HEADERS });
    }

    const url = new URL(request.url);

    // 末尾多一个斜杠也认：设备里的地址是手填的，很容易随手带上 /
    if (url.pathname !== '/api/weather' && url.pathname !== '/api/weather/') {
      return jsonResponse({ code: 404, msg: 'not found' }, 404);
    }

    // 鉴权（可选）：RELAY_KEY 为空时整段跳过 —— 填个域名就能用
    if (RELAY_KEY && url.searchParams.get('key') !== RELAY_KEY) {
      return jsonResponse({ code: 401, msg: 'Unauthorized' }, 401);
    }

    // 参数逐个过白名单
    const upstream = new URL(UPSTREAM);
    for (const [k, v] of url.searchParams) {
      if (ALLOWED_PARAMS.includes(k)) upstream.searchParams.append(k, v);
    }
    if (!upstream.searchParams.get('latitude') || !upstream.searchParams.get('longitude')) {
      return jsonResponse({ code: 400, msg: 'missing latitude/longitude' }, 400);
    }

    try {
      const res = await fetch(upstream.toString(), {
        cf: { cacheTtl: CACHE_TTL, cacheEverything: true },
        signal: AbortSignal.timeout(TIMEOUT_MS)
      });
      if (!res.ok) {
        return jsonResponse({ code: 502, msg: 'upstream status ' + res.status }, 502);
      }
      // 先读成字符串再返回，这样响应自带 Content-Length：
      // 设备端就不必处理 Transfer-Encoding: chunked —— 直连时那种"响应一行一行挤进来"
      // 把超时一路拖长的形态，在这条路径上不会出现。
      const body = await res.text();
      return new Response(body, {
        status: 200,
        headers: {
          ...CORS_HEADERS,
          'Content-Type': 'application/json; charset=utf-8',
          'Cache-Control': 'public, max-age=' + CACHE_TTL
        }
      });
    } catch (e) {
      // 含 AbortSignal 超时与上游不可达。返回非 200，设备会按"本轮失败"处理并等下一个整点。
      return jsonResponse({ code: 502, msg: 'relay failed: ' + (e && e.message ? e.message : e) }, 502);
    }
  }
};
