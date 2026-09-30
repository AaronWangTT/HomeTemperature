#include "DeviceHomepageHandler.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

namespace {

const char HOMEPAGE[] = R"HTPAGE(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<script>(()=>{const param=new URLSearchParams(window.location.hash.slice(1)).get("clawpilotTheme");const theme=param||(window.matchMedia("(prefers-color-scheme: dark)").matches?"dark":"light");document.documentElement.setAttribute("data-theme",theme)})();</script>
<title>HomeTemperature</title><style>
:root {
  color-scheme: light;
  --cp-bg: #f7f4ef;
  --cp-bg-elevated: #fcfbf8;
  --cp-surface: #ffffff;
  --cp-surface-soft: #f5f5f5;
  --cp-border: #dedede;
  --cp-border-strong: #919191;
  --cp-text: #242424;
  --cp-text-muted: #5c5c5c;
  --cp-text-soft: #6f6f6f;
  --cp-accent: #b11f4b;
  --cp-accent-hover: #9a1a41;
  --cp-accent-soft: rgba(177, 31, 75, 0.08);
  --cp-accent-fg: #ffffff;
  --cp-success: #16a34a;
  --cp-danger: #dc2626;
  --cp-warning: #f59e0b;
  --cp-link: #0078d4;
  --cp-shadow: 0 18px 48px rgba(0, 0, 0, 0.12);
  --cp-overlay: rgba(255, 255, 255, 0.8);
  --cp-panel: rgba(255, 255, 255, 0.86);
  --cp-panel-strong: rgba(255, 255, 255, 0.96);
  --cp-sheen: rgba(255, 255, 255, 0.55);
  --cp-highlight: rgba(177, 31, 75, 0.12);
}
html[data-theme="dark"] {
  color-scheme: dark;
  --cp-bg: #3d3b3a;
  --cp-bg-elevated: #343231;
  --cp-surface: #292929;
  --cp-surface-soft: #2e2e2e;
  --cp-border: #474747;
  --cp-border-strong: #5f5f5f;
  --cp-text: #dedede;
  --cp-text-muted: #919191;
  --cp-text-soft: #b0b0b0;
  --cp-accent: #fd8ea1;
  --cp-accent-hover: #fb7b91;
  --cp-accent-soft: rgba(253, 142, 161, 0.14);
  --cp-accent-fg: #1a1a1a;
  --cp-success: #4ade80;
  --cp-danger: #f87171;
  --cp-warning: #fbbf24;
  --cp-link: #4da6ff;
  --cp-shadow: 0 18px 48px rgba(0, 0, 0, 0.32);
  --cp-overlay: rgba(41, 41, 41, 0.88);
  --cp-panel: rgba(41, 41, 41, 0.72);
  --cp-panel-strong: rgba(41, 41, 41, 0.96);
  --cp-sheen: rgba(255, 255, 255, 0.04);
  --cp-highlight: rgba(253, 142, 161, 0.12);
}
*{box-sizing:border-box}body{margin:0;min-width:320px;background:var(--cp-bg);color:var(--cp-text);font-family:"Segoe UI",Aptos,Calibri,-apple-system,BlinkMacSystemFont,sans-serif}.page{width:min(1040px,calc(100% - 32px));margin:auto;padding:32px 0 48px}.top{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:16px}.brand{display:flex;align-items:center;gap:12px}.mark{display:grid;width:42px;height:42px;place-items:center;border-radius:.625rem;background:var(--cp-accent);color:var(--cp-accent-fg);font:bold 18px Consolas,"Courier New",monospace}h1{margin:0;font-size:20px}.sub,.muted{color:var(--cp-text-muted);font-size:13px}.sub{margin-top:3px}.status{display:flex;align-items:center;gap:8px;white-space:nowrap}.dot{width:9px;height:9px;border-radius:50%;background:var(--cp-success)}.dot.bad{background:var(--cp-danger)}.hero{min-height:210px;border:1px solid var(--cp-border);border-radius:16px;background:var(--cp-surface)}.clockbox{display:flex;min-height:210px;flex-direction:column;justify-content:space-between;padding:28px}.eyebrow{color:var(--cp-accent);font-size:12px;font-weight:700;letter-spacing:.08em}.clock{margin:8px 0 2px;font-size:clamp(54px,9vw,82px);font-weight:300;letter-spacing:-.06em;line-height:.95}.date{color:var(--cp-text-muted);font-size:15px}.place{margin-top:24px;font-weight:600}.place:before{color:var(--cp-accent);content:"●";margin-right:8px}.heading{display:flex;align-items:end;justify-content:space-between;margin:28px 0 12px}.heading h2{margin:0;font-size:16px}.heading p{margin:0}.readings{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.card,.details,.footer{border:1px solid var(--cp-border);border-radius:16px;background:var(--cp-surface)}.card{padding:20px}.cardhead{display:flex;justify-content:space-between}.badge{display:grid;width:30px;height:30px;place-items:center;border-radius:50%;background:var(--cp-accent-soft);color:var(--cp-accent);font-weight:700}.value{margin:18px 0 12px;font-size:clamp(34px,6vw,46px);font-weight:350;letter-spacing:-.04em;line-height:1}.unit{margin-left:4px;color:var(--cp-text-muted);font-size:14px}.good{display:flex;align-items:center;gap:7px}.good:before{width:7px;height:7px;border-radius:50%;background:var(--cp-success);content:""}.row{display:grid;grid-template-columns:150px 1fr auto;align-items:center;min-height:52px;gap:16px;padding:10px 18px;border-bottom:1px solid var(--cp-border)}.row:last-child{border:0}.mono{overflow-wrap:anywhere;font:13px Consolas,"Courier New",monospace}.service{display:flex;align-items:center;gap:8px}button,.button{min-height:38px;border:1px solid var(--cp-border);border-radius:.625rem;padding:0 13px;background:var(--cp-surface);color:var(--cp-text);font:600 13px "Segoe UI",Aptos,Calibri,sans-serif;cursor:pointer;text-decoration:none}.copy{min-height:auto;border:0;padding:6px 9px;background:var(--cp-surface-soft);color:var(--cp-link)}button:hover,.button:hover{border-color:var(--cp-border-strong);background:var(--cp-surface-soft)}.primary{border-color:var(--cp-accent);background:var(--cp-accent);color:var(--cp-accent-fg)}.footer{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-top:16px;padding:14px 16px}.actions{display:flex;flex-wrap:wrap;gap:8px}.error{color:var(--cp-danger)}@media(max-width:720px){.page{width:calc(100% - 24px);padding-top:20px}.hero,.clockbox{min-height:180px}.readings{grid-template-columns:1fr}.row{grid-template-columns:110px 1fr auto}.footer{align-items:stretch;flex-direction:column}.actions>*{flex:1;text-align:center}}@media(max-width:460px){.mark{display:none}.clockbox{padding:20px}.row{grid-template-columns:1fr auto;gap:5px 10px}.row .mono,.row .service{grid-column:1}.copy{grid-column:2;grid-row:1/3}}
</style></head><body><main class="page">
<header class="top"><div class="brand"><div class="mark">HT</div><div><h1>HomeTemperature</h1><div class="sub">AZ3166 本地环境监测</div></div></div><div class="status muted"><span class="dot" id="topdot"></span><span id="online">连接中</span></div></header>
<section class="hero"><div class="clockbox"><div><div class="eyebrow">LOCAL TIME</div><div class="clock" id="clock">--:--</div><div class="date" id="date">正在读取时间</div></div><div class="place" id="place">设备位置</div></div></section>
<div class="heading"><h2>当前环境</h2><p class="muted">来自设备板载传感器</p></div>
<section class="readings"><article class="card"><div class="cardhead"><span class="muted">温度</span><span class="badge">T</span></div><div class="value"><span id="temperature">--</span><span class="unit">°C</span></div><div class="good muted" id="temperatureState">等待数据</div></article><article class="card"><div class="cardhead"><span class="muted">相对湿度</span><span class="badge">H</span></div><div class="value"><span id="humidity">--</span><span class="unit">%</span></div><div class="good muted" id="humidityState">等待数据</div></article><article class="card"><div class="cardhead"><span class="muted">大气压力</span><span class="badge">P</span></div><div class="value"><span id="pressure">--</span><span class="unit">hPa</span></div><div class="good muted">当前读数</div></article></section>
<div class="heading"><h2>设备信息</h2><p class="muted">当前网络与固件状态</p></div>
<section class="details"><div class="row"><span class="muted">设备 ID</span><span class="mono" id="deviceId">--</span><button class="copy" data-copy="deviceId">复制</button></div><div class="row"><span class="muted">mDNS 名称</span><span class="mono" id="mdnsName">--</span><button class="copy" data-copy="mdnsName">复制</button></div><div class="row"><span class="muted">IP 地址</span><span class="mono" id="ipAddress">--</span><button class="copy" data-copy="ipAddress">复制</button></div><div class="row"><span class="muted">MAC 地址</span><span class="mono" id="macAddress">--</span><button class="copy" data-copy="macAddress">复制</button></div><div class="row"><span class="muted">HTTP 服务</span><span class="service muted"><span class="dot" id="httpdot"></span><span id="httpstate">连接中</span></span><span></span></div><div class="row"><span class="muted">固件版本</span><span class="mono" id="firmwareVersion">--</span><span></span></div></section>
<footer class="footer"><div class="muted"><div id="updated">尚未读取数据</div><div id="refreshState">正在连接设备</div></div><div class="actions"><button class="primary" id="refresh">立即刷新</button><a class="button" href="/ota">固件更新</a><a class="button" href="/api/telemetry">原始数据</a></div></footer>
</main><script>
const q=id=>document.getElementById(id),state={updated:0,nextAttempt:0,inFlight:false,port:80};function tick(){const n=new Date;q("clock").textContent=n.toLocaleTimeString("zh-CN",{hour:"2-digit",minute:"2-digit",hour12:false});q("date").textContent=n.toLocaleDateString("zh-CN",{year:"numeric",month:"long",day:"numeric",weekday:"long"});if(state.nextAttempt){const left=Math.max(0,Math.ceil((state.nextAttempt-Date.now())/1000));if(!state.inFlight)q("refreshState").textContent=left?left+" 秒后自动刷新":"等待刷新";if(!left&&!state.inFlight)refresh()}}function setOnline(ok){q("online").textContent=ok?"设备在线":"连接异常";q("httpstate").textContent=ok?"正常 · 端口 "+state.port:"无法读取设备数据";q("topdot").classList.toggle("bad",!ok);q("httpdot").classList.toggle("bad",!ok)}function comfort(t,h){q("temperatureState").textContent=t<18?"环境偏冷":t>28?"环境偏热":"体感舒适";q("humidityState").textContent=h<35?"空气偏干":h>70?"湿度偏高":"湿度适宜"}async function json(url){const r=await fetch(url,{cache:"no-store"});if(!r.ok)throw Error(r.status);return r.json()}async function device(){const d=await json("/api/device");["deviceId","mdnsName","ipAddress","macAddress","firmwareVersion"].forEach(k=>q(k).textContent=d[k]||"不可用");state.port=d.httpPort;q("place").textContent=[d.location,d.region].filter(Boolean).join(" · ")||"未设置位置";setOnline(d.online)}async function refresh(){if(state.inFlight)return;state.inFlight=true;q("refresh").disabled=true;q("refreshState").textContent="正在刷新";try{try{const d=await json("/api/telemetry");q("temperature").textContent=Number(d.temperature).toFixed(1);q("humidity").textContent=Number(d.humidity).toFixed(1);q("pressure").textContent=Number(d.pressure).toFixed(1);comfort(d.temperature,d.humidity);state.updated=Date.now();state.nextAttempt=state.updated+60000;q("updated").textContent="数据更新于 "+new Date().toLocaleTimeString("zh-CN",{hour12:false});q("refreshState").classList.remove("error");setOnline(true)}catch(e){state.nextAttempt=Date.now()+10000;q("refreshState").textContent="读取失败，10 秒后重试";q("refreshState").classList.add("error");setOnline(false)}try{await device()}catch(e){console.warn("Device metadata refresh failed",e);if(!state.updated)setOnline(false)}}finally{state.inFlight=false;q("refresh").disabled=false}}function legacyCopy(value){const area=document.createElement("textarea");area.value=value;area.style.position="fixed";area.style.opacity="0";document.body.appendChild(area);area.select();let copied=false;try{copied=document.execCommand("copy")}catch(e){console.warn("Copy fallback failed",e)}area.remove();return copied}async function copy(button){const value=q(button.dataset.copy).textContent;let copied=false;try{if(navigator.clipboard&&window.isSecureContext){await navigator.clipboard.writeText(value);copied=true}else copied=legacyCopy(value)}catch(e){console.warn("Clipboard API failed",e);copied=legacyCopy(value)}button.textContent=copied?"已复制":"复制失败";setTimeout(()=>button.textContent="复制",1500)}q("refresh").onclick=refresh;document.querySelectorAll("[data-copy]").forEach(b=>b.onclick=()=>copy(b));document.addEventListener("visibilitychange",()=>{if(!document.hidden&&(!state.nextAttempt||Date.now()>=state.nextAttempt))refresh()});tick();refresh();setInterval(tick,1000);
</script></body></html>)HTPAGE";

static_assert(sizeof(HOMEPAGE) < 20000, "Homepage exceeds its flash budget");
const char HOMEPAGE_CACHE_CONTROL[] = "no-cache";
const char API_CACHE_CONTROL[] = "no-store";

uint32_t homepageHash() {
    uint32_t hash = 2166136261UL;
    for (size_t index = 0; index < sizeof(HOMEPAGE) - 1; ++index) {
        hash ^= static_cast<uint8_t>(HOMEPAGE[index]);
        hash *= 16777619UL;
    }
    return hash;
}

bool appendBytes(
    char *body,
    size_t bodySize,
    size_t &length,
    const char *value,
    size_t valueLength) {
    if (body == NULL || value == NULL || length >= bodySize ||
        valueLength > bodySize - length - 1) {
        return false;
    }
    memcpy(body + length, value, valueLength);
    length += valueLength;
    body[length] = '\0';
    return true;
}

bool appendLiteral(
    char *body, size_t bodySize, size_t &length, const char *value) {
    return appendBytes(body, bodySize, length, value, strlen(value));
}

bool appendJsonString(
    char *body, size_t bodySize, size_t &length, const char *value) {
    if (!appendLiteral(body, bodySize, length, "\"")) {
        return false;
    }
    const unsigned char *current =
        reinterpret_cast<const unsigned char *>(value == NULL ? "" : value);
    while (*current != '\0') {
        char escaped[7];
        size_t escapedLength = 1;
        escaped[0] = static_cast<char>(*current);
        if (*current == '"' || *current == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(*current);
            escapedLength = 2;
        } else if (*current < 0x20) {
            int count = snprintf(
                escaped, sizeof(escaped), "\\u%04X",
                static_cast<unsigned int>(*current));
            if (count != 6) {
                return false;
            }
            escapedLength = 6;
        }
        if (!appendBytes(body, bodySize, length, escaped, escapedLength)) {
            return false;
        }
        ++current;
    }
    return appendLiteral(body, bodySize, length, "\"");
}

}

DeviceHomepageHandler::DeviceHomepageHandler(
    LocalHttpHandler &fallback,
    const char *hostname,
    const char *location,
    const char *region,
    const char *firmwareVersion,
    uint16_t httpPort)
    : fallback_(fallback),
      hostname_(hostname),
      location_(location),
      region_(region),
      firmwareVersion_(firmwareVersion),
      httpPort_(httpPort),
      address_(0),
      connected_(false) {
    deviceId_[0] = '\0';
    macAddress_[0] = '\0';
    snprintf(
        homepageEtag_,
        sizeof(homepageEtag_),
        "\"homepage-%08lX\"",
        static_cast<unsigned long>(homepageHash()));
}

bool DeviceHomepageHandler::begin(const char *deviceId) {
    if (deviceId == NULL || strlen(deviceId) >= sizeof(deviceId_)) {
        return false;
    }
    std::lock_guard<rtos::Mutex> lock(stateMutex_);
    strcpy(deviceId_, deviceId);
    return true;
}

void DeviceHomepageHandler::updateNetwork(
    bool connected,
    uint32_t address,
    const uint8_t macAddress[6]) {
    std::lock_guard<rtos::Mutex> lock(stateMutex_);
    connected_ = connected && address != 0;
    address_ = connected_ ? address : 0;
    if (macAddress != NULL) {
        snprintf(
            macAddress_,
            sizeof(macAddress_),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            static_cast<unsigned int>(macAddress[0]),
            static_cast<unsigned int>(macAddress[1]),
            static_cast<unsigned int>(macAddress[2]),
            static_cast<unsigned int>(macAddress[3]),
            static_cast<unsigned int>(macAddress[4]),
            static_cast<unsigned int>(macAddress[5]));
    }
}

bool DeviceHomepageHandler::exactGet(
    const char *requestLine, const char *path) {
    if (requestLine == NULL || path == NULL) {
        return false;
    }

    char expected[96];
    int length = snprintf(
        expected, sizeof(expected), "GET %s HTTP/1.1", path);
    if (length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
        strcmp(requestLine, expected) == 0) {
        return true;
    }
    length = snprintf(expected, sizeof(expected), "GET %s HTTP/1.0", path);
    return length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
        strcmp(requestLine, expected) == 0;
}

bool DeviceHomepageHandler::etagMatches(
    const char *ifNoneMatch, const char *etag) {
    if (ifNoneMatch == NULL || etag == NULL) {
        return false;
    }
    const size_t etagLength = strlen(etag);
    const char *current = ifNoneMatch;
    while (*current != '\0') {
        while (*current == ' ' || *current == '\t' || *current == ',') {
            ++current;
        }
        const char *token = current;
        while (*current != '\0' && *current != ',') {
            ++current;
        }
        const char *end = current;
        while (end > token && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        if (end - token == 1 && token[0] == '*') {
            return true;
        }
        if (end - token >= 2 && token[0] == 'W' && token[1] == '/') {
            token += 2;
        }
        if (static_cast<size_t>(end - token) == etagLength &&
            memcmp(token, etag, etagLength) == 0) {
            return true;
        }
    }
    return false;
}

LocalHttpResponse DeviceHomepageHandler::deviceInfo(
    char *body, size_t bodySize) {
    char deviceId[sizeof(deviceId_)];
    char macAddress[sizeof(macAddress_)];
    uint32_t address;
    bool connected;
    {
        std::lock_guard<rtos::Mutex> lock(stateMutex_);
        strcpy(deviceId, deviceId_);
        strcpy(macAddress, macAddress_);
        address = address_;
        connected = connected_;
    }

    char mdnsName[80];
    int mdnsLength = snprintf(
        mdnsName,
        sizeof(mdnsName),
        "%s.local",
        hostname_ == NULL ? "" : hostname_);
    char ipAddress[16];
    int ipLength = connected
        ? snprintf(
              ipAddress,
              sizeof(ipAddress),
              "%lu.%lu.%lu.%lu",
              static_cast<unsigned long>((address >> 24) & 0xFF),
              static_cast<unsigned long>((address >> 16) & 0xFF),
              static_cast<unsigned long>((address >> 8) & 0xFF),
              static_cast<unsigned long>(address & 0xFF))
        : snprintf(ipAddress, sizeof(ipAddress), "%s", "");
    if (mdnsLength <= 0 ||
        static_cast<size_t>(mdnsLength) >= sizeof(mdnsName) ||
        ipLength < 0 ||
        static_cast<size_t>(ipLength) >= sizeof(ipAddress)) {
        const char error[] = "{\"error\":\"device information unavailable\"}";
        if (body == NULL || sizeof(error) > bodySize) {
            return {"500 Internal Server Error", "application/json", 0};
        }
        memcpy(body, error, sizeof(error));
        return {
            "500 Internal Server Error",
            "application/json",
            sizeof(error) - 1
        };
    }

    size_t length = 0;
    bool formatted =
        appendLiteral(body, bodySize, length, "{\"deviceId\":") &&
        appendJsonString(body, bodySize, length, deviceId) &&
        appendLiteral(body, bodySize, length, ",\"mdnsName\":") &&
        appendJsonString(body, bodySize, length, mdnsName) &&
        appendLiteral(body, bodySize, length, ",\"ipAddress\":") &&
        appendJsonString(body, bodySize, length, ipAddress) &&
        appendLiteral(body, bodySize, length, ",\"macAddress\":") &&
        appendJsonString(body, bodySize, length, macAddress) &&
        appendLiteral(body, bodySize, length, ",\"firmwareVersion\":") &&
        appendJsonString(body, bodySize, length, firmwareVersion_) &&
        appendLiteral(body, bodySize, length, ",\"location\":") &&
        appendJsonString(body, bodySize, length, location_) &&
        appendLiteral(body, bodySize, length, ",\"region\":") &&
        appendJsonString(body, bodySize, length, region_);
    char suffix[48];
    int suffixLength = snprintf(
        suffix,
        sizeof(suffix),
        ",\"httpPort\":%u,\"online\":%s}",
        static_cast<unsigned int>(httpPort_),
        connected ? "true" : "false");
    formatted = formatted && suffixLength > 0 &&
        static_cast<size_t>(suffixLength) < sizeof(suffix) &&
        appendBytes(
            body,
            bodySize,
            length,
            suffix,
            static_cast<size_t>(suffixLength));
    if (!formatted) {
        if (body != NULL && bodySize > 0) {
            body[0] = '\0';
        }
        return {"500 Internal Server Error", "application/json", 0};
    }
    return {"200 OK", "application/json", length};
}

LocalHttpResponse DeviceHomepageHandler::handle(
    const char *requestLine,
    char *body,
    size_t bodySize) {
    LocalHttpRequest request = {};
    request.requestLine = requestLine;
    return handleRequest(request, body, bodySize);
}

LocalHttpResponse DeviceHomepageHandler::handleRequest(
    const LocalHttpRequest &request,
    char *body,
    size_t bodySize) {
    const char *requestLine = request.requestLine;
    if (exactGet(requestLine, "/")) {
        if (request.ifNoneMatchCount > 0 &&
            etagMatches(request.ifNoneMatch, homepageEtag_)) {
            return {
                "304 Not Modified",
                "text/html; charset=utf-8",
                0,
                NULL,
                NULL,
                false,
                NULL,
                HOMEPAGE_CACHE_CONTROL,
                homepageEtag_
            };
        }
        return {
            "200 OK",
            "text/html; charset=utf-8",
            sizeof(HOMEPAGE) - 1,
            NULL,
            NULL,
            false,
            HOMEPAGE,
            HOMEPAGE_CACHE_CONTROL,
            homepageEtag_
        };
    }
    if (exactGet(requestLine, "/api/device")) {
        LocalHttpResponse response = deviceInfo(body, bodySize);
        response.cacheControl = API_CACHE_CONTROL;
        return response;
    }
    LocalHttpResponse response =
        fallback_.handleRequest(request, body, bodySize);
    if (exactGet(requestLine, "/api/telemetry")) {
        response.cacheControl = API_CACHE_CONTROL;
    }
    return response;
}

bool DeviceHomepageHandler::requiresRequestMetadata(const char *requestLine) {
    return fallback_.requiresRequestMetadata(requestLine);
}
