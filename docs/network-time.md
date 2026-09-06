# Network Time: IP Geolocation + NTP · 网络时间：IP 定位 + NTP 同步

> [← Back to README](../README.md) · ESPaperPlay documentation
>
> [← 返回 README](../README.md) · ESPaperPlay 项目文档

<p align="center">
  <a href="#en">English</a> · <a href="#zh">简体中文</a>
</p>

---

<a id="en"></a>
## English

### Network Time: IP Geolocation + NTP Sync

Three independent services, chained at boot by the `nettime` application
(`components/applications/nettime`) once WiFi is up in STA mode:

1. **Public IP** — `netip` service (`components/services/netip`) calls the UAPI
   *Query My IP* endpoint (`GET https://uapis.cn/api/v1/network/myip`) over
   HTTPS (validated with the ESP-IDF built-in CA certificate bundle) and
   returns the device's public egress IP (`espaperplay_netip_query()`).
2. **Geolocation** — `geoip` service (`components/services/geoip`) calls the
   UAPI *Query IP* endpoint (`GET https://uapis.cn/api/v1/network/ipinfo?ip=…`)
   with **the same IP** from step 1 and returns country/province/city, ISP,
   ASN, latitude/longitude and the IANA timezone. It prefers the
   `source=commercial` result (adds the `time_zone` field, e.g.
   `Asia/Shanghai`) and falls back to the standard query automatically
   (`espaperplay_geoip_query()`).
3. **Timezone + NTP** — `clock` service (`components/services/clock`) applies
   the timezone reported by geolocation (`setenv("TZ")` + `tzset()`, persisted
   to NVS so it survives reboots), then starts SNTP through `esp_netif_sntp`
   with three NTP servers (`ntp.aliyun.com`, `cn.pool.ntp.org`,
   `pool.ntp.org`) and waits for the first sync
   (`espaperplay_clock_set_timezone()` / `espaperplay_clock_ntp_start()` /
   `espaperplay_clock_ntp_wait_sync()`).

The three services are fully independent — each can be called standalone with
any IP / timezone; only the `nettime` app wires them together. Requires
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (HTTPS) and
`CONFIG_LWIP_SNTP_MAX_SERVERS=3` (multi-server NTP), both preset in
`sdkconfig.defaults`. If the network is down at boot, the time sync retries
automatically once connectivity returns.

**Query caching** — to avoid repeated API calls, both query services cache
results in RAM: `netip` keeps a single entry (the device's own public IP) and
`geoip` keeps the 4 most recent IPs (FIFO eviction). Default TTL is 1 hour;
hits are served without any network request, and caches are only refreshed
after expiry or an explicit `espaperplay_netip_cache_clear()` /
`espaperplay_geoip_cache_clear()`. Adjust with
`espaperplay_netip_set_cache_ttl_ms()` / `espaperplay_geoip_set_cache_ttl_ms()`
(0 disables caching).

---

<a id="zh"></a>
## 简体中文

### 网络时间：IP 定位 + NTP 同步

三个相互独立的服务，由 `nettime` 应用（`components/applications/nettime`）
在 WiFi 以 STA 模式联网后串联执行：

1. **获取本机公网 IP** —— `netip` 服务（`components/services/netip`）调用
   UAPI「查询我的 IP」接口（`GET https://uapis.cn/api/v1/network/myip`），
   HTTPS 请求（使用 ESP-IDF 内置 CA 证书包校验服务器证书），返回设备出口
   公网 IP（`espaperplay_netip_query()`）。
2. **相同 IP 查询地理位置** —— `geoip` 服务（`components/services/geoip`）
   用**第 1 步得到的同一个 IP** 调用 UAPI「IP 查询」接口
   （`GET https://uapis.cn/api/v1/network/ipinfo?ip=…`），返回国家 / 省份 /
   城市、运营商、ASN、经纬度与 IANA 时区。优先使用 `source=commercial`
   商业级查询（附带 `time_zone` 字段，如 `Asia/Shanghai`），失败时自动回退
   标准查询（`espaperplay_geoip_query()`）。
3. **设置时区 + NTP 同步** —— `clock` 服务（`components/services/clock`）
   应用地理位置返回的时区（`setenv("TZ")` + `tzset()`，持久化到 NVS，
   重启后自动恢复），再通过 `esp_netif_sntp` 启动 SNTP，默认配置三个 NTP
   服务器（`ntp.aliyun.com`、`cn.pool.ntp.org`、`pool.ntp.org`）并等待首次
   同步完成（`espaperplay_clock_set_timezone()` /
   `espaperplay_clock_ntp_start()` / `espaperplay_clock_ntp_wait_sync()`）。

三个服务彼此独立、可单独调用（可传入任意 IP / 时区），仅由 `nettime`
应用负责编排串联。依赖 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`（HTTPS）与
`CONFIG_LWIP_SNTP_MAX_SERVERS=3`（多服务器 NTP），两者已写入
`sdkconfig.defaults`。开机时若网络不可用，联网恢复后会自动重试对时。

**查询缓存** —— 为避免重复请求，两个查询服务均把结果缓存在内存中：
`netip` 缓存单条（设备自身公网 IP），`geoip` 缓存最近查询的 4 个 IP
（满时淘汰最旧）。默认有效期 1 小时，命中时直接返回缓存、不发网络请求，
仅过期或调用 `espaperplay_netip_cache_clear()` /
`espaperplay_geoip_cache_clear()` 后强制重新查询；有效期可通过
`espaperplay_netip_set_cache_ttl_ms()` / `espaperplay_geoip_set_cache_ttl_ms()`
调整（设为 0 禁用缓存）。
