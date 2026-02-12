
Add your WiFi SSID/password and the SmartVita API token to `include/secrets.h` and do not commit that file (already in gitignore).

```
#pragma once
const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
const char ACCESS_TOKEN[] = "YOUR_SMARTVITA_ACCESS_TOKEN";
```

## Overview
- ESP32-C6 as MQTT-to-HTTP gateway (remote control of LAN devices)  
- Single MQTT topic for both requests and responses (request/response multiplexing)  

## Transport & security
- MQTT over TLS (port 8883) with CA verification (`setCACert`)[1]
- Time sync via NTP required for certificate date validation
- Avoid long/blocking work inside MQTT callback; queue + process in main loop

## Topic
- Topic: `t/LeoTestID`  
- Same topic for `req` and `res`; clients filter by `type`, `id`, `from`  

## Message schema (JSON)
- Root fields: `v`, `type`, `id`, `from`, optional `to`, `http`  
- `type`: `req` | `res`  
- `id`: correlation id (one request ↔ one response)  
- `from`: sender id (e.g., `web-...`, `esp-<mac>`)  
- `http`: `method`, `url`, optional `headers`, optional `body_b64` 
```
Request:
{
  "v": 1,
  "type": "req",
  "id": "toggle-002",
  "from": "web-hivemq",
  "to": "esp-3C:84:27:AA:BB:CC",
  "http": {
    "method": "GET",
    "url": "http://192.168.0.124/relay/0?turn=toggle",
    "headers": {
      "Accept": "application/json"
    },
    "body_b64": ""
  }
}
Response:
{
  "v": 1,
  "type": "res",
  "id": "toggle-002",
  "from": "esp-3C:84:27:AA:BB:CC",
  "ok": true,
  "status": 200,
  "body_b64": "eyJpc29uIjp0cnVlfQ==",
  "error": null
}

``` 

## Body encoding
- `body_b64`: Base64-encoded HTTP body (request/response)[2]
- Rationale: binary-safe payload inside JSON string (non-UTF8, null bytes, arbitrary bytes)[2]
- Trade-off: payload size overhead vs. robustness[3]

## HTTP execution rules (ESP)
- Allowlist for URLs (e.g., `http://192.168.*` / `http://10.*`)  
- HTTP client: `HTTPClient.begin(WiFiClient, url)` pattern for plain HTTP
- Default header injection when missing (e.g., `Content-Type: application/json`) via key checks (`containsKey`)  

## Example request (Shelly toggle)
- Publish JSON (`type=req`) with `http.method=GET` and `http.url=http://192.168.0.124/relay/0?turn=toggle`[4]

## Example response
- `type=res`, same `id`, `status` (HTTP code), `body_b64` (Base64 body), `from=esp-<mac>`  

## Client behavior (Web app)
- Generate `id` per request; wait for matching `res.id` before next request (sequential mode)  
- Ignore own messages and non-matching `type`/`id`/`from`  

## PlatformIO deps
- `PubSubClient` (MQTT)
- `ArduinoJson` (JSON parsing/serialization)  
- Optional: serial monitor speed/config for debugging

# Known Errors
## No Console logs
Restart PC