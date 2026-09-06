# Weather · 天气

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Weather Service (QWeather / 和风天气)

The `weather` service (`components/services/weather`) fetches weather data from
the official [QWeather Web API v7](https://dev.qweather.com/docs/api/) over
HTTPS (ESP-IDF CA certificate bundle, authenticated with the `X-QW-Api-Key`
header). It implements:

- **Real-time weather** — `v7/weather/now`;
- **Daily forecast** — `v7/weather/3d` and `v7/weather/7d` (7-day with
  automatic 3-day fallback for subscriptions that do not include it);
- **Hourly forecast** — `v7/weather/24h` (next 24 hours);
- **Minute-level precipitation** — `v7/minutely/5m` (next 2 hours, China
  mainland; the endpoint only accepts "lon,lat" coordinates on the new
  platform, so the service resolves the LocationID back to coordinates via
  GeoAPI before requesting);
- **Weather alerts** — `v7/warning/now`;
- **Weather indices** — `v7/indices/1d` (all types);
- **Air quality** — `v7/air/now` (China mainland);
- **Astronomy** — `v7/astronomy/sun` + `v7/astronomy/moon`
  (sunrise/sunset, moonrise/moonset, moon phase; the endpoints require the
  `date=yyyyMMdd` parameter, filled with the local date; timestamps are
  returned in full ISO form and truncated to `HH:MM` for display);
- **GeoAPI** — city-name / "lon,lat" reverse lookup (default public host
  `geoapi.qweather.com/v2/city/lookup`; with a custom API Host configured the
  new-style `{host}/geo/v2/city/lookup` path is used; a manual LocationID is
  used as-is).

QWeather responses are gzip-compressed by default; the service detects the
gzip magic bytes and decompresses them with the `espressif/zlib` managed
component (see `components/services/weather/idf_component.yml`).

**Configuration** — the API key, an optional location and an optional custom
API Host are persisted in NVS and can be set from the web console (*Weather*
section): `weather_api_key` (required), `weather_location` (optional
LocationID / city name / "lon,lat" like "116.41,39.92" — "lat,lon" is
auto-detected and swapped; empty = auto-locate) and `weather_api_host`
(optional; QWeather is phasing out the public hosts devapi/geoapi.qweather.com
from 2026 — copy your own API Host from the console settings).
Without a manual location the service auto-locates: public IP (`netip`) →
geolocation (`geoip`) → reverse GeoAPI lookup of the coordinates (lon,lat,
2-decimal precision, cached for 24 h).

**Operation** — `espaperplay_weather_start()` runs a background task that
waits for STA connectivity and then refreshes the in-RAM snapshot
(`espaperplay_weather_get_snapshot()`) on a 10-minute cycle. Every API has its
own TTL cache (now/minutely/warnings 10 min, hourly 30 min, daily/air 60 min,
indices/astronomy 6 h) so expired data is fetched independently; the web
console's *Refresh now* button or `espaperplay_weather_request_refresh()`
wakes the task early. QWeather business codes are surfaced through
`espaperplay_weather_get_status()` (e.g. `401` = invalid key, `403` = quota
exceeded). The service's ~52 KB of static caches live in **PSRAM (lazily
allocated)** to keep internal DRAM free.

### Weather Screen

`screen_weather` (home → 天气) renders the snapshot on three swipeable
sub-pages (dots at the bottom; swipes in chart areas are left to LVGL
scrolling):

- **子页 0「实时」** — large current temperature (+ feels-like), today's
  high/low, weather icon, an alert banner (tap for a detail card), and the
  next 24 h temperature curve;
- **子页 1「7 天 + 详情」** — 7-day high/low dual temperature curve with
  labels, daily details;
- **子页 2「天文 + 指数」** — sun & moon rise/set arcs with the sun/moon icon
  positioned by the current time (the moon uses the **real moon phase** from
  `moonPhase.icon`, codes 800–807), plus the life-indices list. Days without
  an actual moonrise/moonset (a normal astronomical phenomenon, 1–2 days per
  month) show `--` and backfill the neighboring day's data so the arc stays
  complete.

A "最近更新" (last updated) tag shows snapshot freshness; stale data nudges
the background task to refresh immediately.

---

<a id="zh"></a>
## 简体中文

### 天气服务（和风天气 QWeather）

`weather` 服务（`components/services/weather`）通过官方
[和风天气 Web API v7](https://dev.qweather.com/docs/api/)（HTTPS + ESP-IDF
内置 CA 证书包校验，`X-QW-Api-Key` 请求头认证）获取天气数据，实现：

- **实时天气** —— `v7/weather/now`；
- **逐日天气预报** —— `v7/weather/3d` 与 `v7/weather/7d`（7 日预报失败时
  自动回退 3 日，适配不含 7 日预报的订阅）；
- **逐小时预报** —— `v7/weather/24h`（未来 24 小时）；
- **分钟级降水** —— `v7/minutely/5m`（未来 2 小时逐分钟降水强度，
  仅中国大陆；新平台该接口只接受"经度,纬度"坐标，服务会自动把
  LocationID 经 GeoAPI 反查为坐标后再请求）；
- **气象灾害预警** —— `v7/warning/now`；
- **天气指数** —— `v7/indices/1d`（全部类型）；
- **空气质量** —— `v7/air/now`（仅中国大陆）；
- **天文** —— `v7/astronomy/sun` + `v7/astronomy/moon`（日出日落、
  月升月落、月相；接口必选 `date=yyyyMMdd` 参数，自动填本地当天日期；
  返回时间为完整时间戳，展示时截取 `HH:MM`）；
- **GeoAPI 城市查询** —— 城市名 / "经度,纬度" 反查 LocationID（默认公共地址
  `geoapi.qweather.com/v2/city/lookup`；配置自定义 API Host 后使用
  `{host}/geo/v2/city/lookup`）。

和风天气 API 默认以 gzip 压缩响应，服务检测到 gzip 魔数后用
`espressif/zlib` 托管组件解压（见 `components/services/weather/idf_component.yml`）。

**配置** —— API Key 与可选位置持久化在 NVS，可在 Web 管理控制台「天气
设置」区配置：`weather_api_key`（必填）、`weather_location`（可选
LocationID / 城市名 / "经度,纬度"，如 "116.41,39.92"；按习惯写
"纬度,经度" 会自动交换；留空 = 自动定位）与 `weather_api_host`（可选；
和风天气 2026 年起逐步停止公共地址 devapi/geoapi.qweather.com，建议在
控制台-设置获取自己的 API Host 填入）。未配置位置时服务自动定位：公网
IP（`netip`）→ 地理位置（`geoip`）→ GeoAPI 经纬度反查 LocationID
（结果缓存 24 小时）。

**运行** —— `espaperplay_weather_start()` 启动后台任务：等待 STA 联网后
每 10 分钟把全部数据刷新进内存快照（`espaperplay_weather_get_snapshot()`）。
各接口拥有独立 TTL 缓存（实时 / 分钟级 / 预警 10 分钟、逐小时 30 分钟、
预报 / 空气 60 分钟、指数 / 天文 6 小时），按需单独过期；管理页「立即
刷新」按钮或 `espaperplay_weather_request_refresh()` 可提前唤醒任务。
QWeather 业务码通过 `espaperplay_weather_get_status()` 对外暴露
（如 401 = Key 无效、403 = 配额超限）。服务的约 52KB 静态缓存位于
**PSRAM（惰性分配）**，为内部 DRAM 省空间。

### 天气页

`screen_weather`（主界面 → 天气）把快照渲染为三个可滑动子页（底部圆点
指示；图表滚动区内的滑动让给 LVGL 滚动）：

- **子页 0「实时」**——大字号当前气温（+ 体感）、今日高低温、天气图标、
  预警长条（点击弹详情卡片）、未来 24h 气温曲线；
- **子页 1「7 天 + 详情」**——未来 7 天最高 / 最低气温双曲线（带数值
  标注）与逐日详情；
- **子页 2「天文 + 指数」**——日出日落 / 月出月落弧线，太阳 / 月亮图标按
  当前时间在弧线上定位（月亮按 API 返回的**真实月相**显示，
  `moonPhase.icon` 800–807 段），下方为生活指数列表。当天确无月出 /
  月落（正常天文现象，每月 1–2 天）显示「--」并回填相邻日期数据，
  弧线保持完整。

页面提供「最近更新」动态标签显示快照新旧；数据过期时自动催促后台任务
立即刷新。
