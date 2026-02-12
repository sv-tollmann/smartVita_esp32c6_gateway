// #include <Arduino.h>
// #include <WiFi.h>
// #include <HTTPClient.h>
// #include <WiFiClientSecure.h>
// #include <PubSubClient.h>
// #include <ArduinoJson.h>
// #include <time.h>
// #include <memory>
// #include <WiFi.h>
// #include <WiFiManager.h>
// #include "mbedtls/base64.h"

// #include <Arduino.h>
// #include <SPI.h>
// #include <Arduino_GFX_Library.h>

// static constexpr int PIN_MOSI = 6;
// static constexpr int PIN_SCLK = 7;
// static constexpr int PIN_CS = 14;
// static constexpr int PIN_DC = 15;
// static constexpr int PIN_RST = 21;
// static constexpr int PIN_BL = 22;

// #include "secrets.h"
// #include "cert.h"

// const char *MQTT_SERVER = "a5247c1ae5634398b09808b959fd47e9.s1.eu.hivemq.cloud";
// const uint16_t MQTT_PORT = 8883;
// const char *MQTT_TOPIC_SUB = "t/LeoTestID";

// WiFiClientSecure espClient;
// PubSubClient mqttClient(espClient);

// static const uint16_t MQTT_BUFFER_SIZE = 4096;
// static const uint32_t HTTP_TIMEOUT_MS = 15000;

// // JSON Speicher (anpassen, wenn du größere Bodies erwartest)
// static const size_t JSON_DOC_SIZE = 4096;
// static const size_t MAX_BODY_DECODED = 2048; // Schutz gegen riesige Bodies

// String espId; // z.B. "esp-<mac>"

// String pendingMsg;
// bool hasPending = false;

// // Generic HW SPI bus (uses Arduino SPI)
// Arduino_DataBus *bus = new Arduino_HWSPI(PIN_DC, PIN_CS); // documented simple SPI bus [page:0]

// Arduino_GFX *gfx = new Arduino_ST7789(
//     bus, PIN_RST,
//     0 /* rotation */, false /* IPS */,
//     172 /* width */, 320 /* height */,
//     34 /* col_offset1 */, 0 /* row_offset1 */,
//     34 /* col_offset2 */, 0 /* row_offset2 */
// );

// // ---------- helpers: time ----------
// void syncTimeOrWarn()
// {
//   configTime(0, 0, "pool.ntp.org", "time.nist.gov");

//   time_t now = time(nullptr);
//   uint32_t start = millis();
//   while (now < 1700000000 && (millis() - start) < 15000)
//   { // ~2023+
//     delay(200);
//     now = time(nullptr);
//   }

//   if (now < 1700000000)
//   {
//     Serial.println("WARN: NTP Zeit nicht gesetzt -> TLS Zertifikatsprüfung kann scheitern.");
//   }
//   else
//   {
//     Serial.println("Zeit via NTP gesetzt.");
//   }
// }

// // ---------- helpers: base64 ----------
// bool b64DecodeToString(const char *in, String &out, size_t maxOut)
// {
//   out = "";
//   if (!in)
//     return true;
//   size_t inLen = strlen(in);
//   if (inLen == 0)
//     return true;

//   size_t bufLen = (inLen * 3) / 4 + 8;
//   if (bufLen > maxOut)
//     return false;

//   std::unique_ptr<uint8_t[]> buf(new uint8_t[bufLen]);
//   size_t outLen = 0;

//   int rc = mbedtls_base64_decode(buf.get(), bufLen, &outLen,
//                                  (const unsigned char *)in, inLen);
//   if (rc != 0)
//     return false;

//   out.reserve(outLen);
//   for (size_t i = 0; i < outLen; i++)
//     out += (char)buf[i];
//   return true;
// }

// String b64Encode(const uint8_t *data, size_t len)
// {
//   if (!data || len == 0)
//     return "";
//   size_t outLen = 0;
//   size_t bufLen = 4 * ((len + 2) / 3) + 1;

//   std::unique_ptr<uint8_t[]> buf(new uint8_t[bufLen]);
//   int rc = mbedtls_base64_encode(buf.get(), bufLen, &outLen, data, len);
//   if (rc != 0)
//     return "";

//   return String((char *)buf.get()).substring(0, outLen);
// }

// // ---------- helpers: publish ----------
// void publishJson(const JsonDocument &doc)
// {
//   String out;
//   serializeJson(doc, out);
//   mqttClient.publish(MQTT_TOPIC_SUB, out.c_str());
// }

// void publishError(const char *reqId, const char *err, int status = -1)
// {
//   StaticJsonDocument<768> res;
//   res["v"] = 1;
//   res["type"] = "res";
//   res["id"] = reqId ? reqId : "";
//   res["from"] = espId;
//   res["ok"] = false;
//   res["status"] = status;
//   res["error"] = err ? err : "error";
//   res["body_b64"] = "";
//   publishJson(res);
// }

// // ---------- allowlist (Sicherheit) ----------
// bool isUrlAllowed(const String &url)
// {
//   if (!url.startsWith("http://"))
//     return false;
//   if (url.startsWith("http://192.168."))
//     return true;
//   if (url.startsWith("http://10."))
//     return true;
//   return false;
// }

// // ---------- HTTP execute ----------
// bool doHttpRequest(JsonObjectConst http, int &statusOut, String &respOut)
// {
//   Serial.println("---- doHttpRequest ----");

//   statusOut = -1;
//   respOut = "";

//   const char *method = http["method"] | "";
//   const char *urlC = http["url"] | "";
//   const char *bodyB64 = http["body_b64"] | "";

//   Serial.print("method=");
//   Serial.println(method);
//   Serial.print("url=");
//   Serial.println(urlC);

//   if (method[0] == '\0' || urlC[0] == '\0')
//   {
//     respOut = "Missing method/url";
//     return false;
//   }

//   String url(urlC);
//   if (!isUrlAllowed(url))
//   {
//     respOut = "URL not allowed";
//     return false;
//   }

//   String body;
//   if (!b64DecodeToString(bodyB64, body, MAX_BODY_DECODED))
//   {
//     respOut = "Body base64 decode failed/too large";
//     return false;
//   }

//   HTTPClient client;
//   client.setTimeout(HTTP_TIMEOUT_MS);

//   WiFiClient net; // unverschlüsseltes HTTP im LAN
//   if (!client.begin(net, url))
//   {
//     respOut = "HTTP begin() failed";
//     return false;
//   }
//   Serial.println("HTTP begin OK");

//   // optionale Header
//   if (http["headers"].is<JsonObjectConst>())
//   {
//     JsonObjectConst headersObj = http["headers"].as<JsonObjectConst>();
//     for (JsonPairConst kv : headersObj)
//     {
//       const char *k = kv.key().c_str();
//       const char *v = kv.value().as<const char *>();
//       if (k && v)
//         client.addHeader(k, v);
//     }
//   }

//   String m(method);
//   m.toUpperCase();

//   int code = -1;
//   String bodyResp;

//   if (m == "GET")
//   {
//     Serial.print("HTTP -> ");
//     Serial.println(url);
//     code = client.GET();
//     bodyResp = client.getString();
//   }
//   else if (m == "POST")
//   {
//     bool hasHeaders = http["headers"].is<JsonObjectConst>();
//     JsonObjectConst headersObj = hasHeaders ? http["headers"].as<JsonObjectConst>() : JsonObjectConst();
//     if (!hasHeaders || !headersObj.containsKey("Content-Type"))
//     {
//       client.addHeader("Content-Type", "application/json");
//     }
//     code = client.POST((uint8_t *)body.c_str(), body.length());
//     bodyResp = client.getString();
//   }
//   else if (m == "PUT")
//   {
//     bool hasHeaders = http["headers"].is<JsonObjectConst>();
//     JsonObjectConst headersObj = hasHeaders ? http["headers"].as<JsonObjectConst>() : JsonObjectConst();
//     if (!hasHeaders || !headersObj.containsKey("Content-Type"))
//     {
//       client.addHeader("Content-Type", "application/json");
//     }
//     code = client.PUT((uint8_t *)body.c_str(), body.length());
//     bodyResp = client.getString();
//   }
//   else if (m == "DELETE")
//   {
//     code = client.sendRequest("DELETE", (uint8_t *)body.c_str(), body.length());
//     bodyResp = client.getString();
//   }
//   else
//   {
//     client.end();
//     respOut = "Unsupported HTTP method";
//     return false;
//   }

//   Serial.print("HTTP code: ");
//   Serial.println(code);
//   Serial.print("HTTP resp len: ");
//   Serial.println(bodyResp.length());
//   Serial.println(bodyResp);

//   statusOut = code;
//   respOut = bodyResp;

//   client.end();
//   return (code > 0);
// }

// // ---------- MQTT request handler ----------
// void handleReq(const JsonDocument &req)
// {
//   Serial.println("==== handleReq ====");

//   JsonObjectConst root = req.as<JsonObjectConst>();
//   if (root.isNull())
//   {
//     Serial.println("Abort: root is not an object");
//     return;
//   }

//   const char *id = root["id"] | "";
//   Serial.print("id=");
//   Serial.println(id);
//   if (id[0] == '\0')
//   {
//     Serial.println("Abort: missing id");
//     return;
//   }

//   if (!root["http"].is<JsonObjectConst>())
//   {
//     Serial.println("Abort: missing http object");
//     publishError(id, "Missing http object");
//     return;
//   }

//   JsonObjectConst httpObj = root["http"].as<JsonObjectConst>();

//   const char *method = httpObj["method"] | "";
//   const char *urlC = httpObj["url"] | "";
//   Serial.print("method=");
//   Serial.println(method);
//   Serial.print("url=");
//   Serial.println(urlC);

//   int httpStatus = -1;
//   String httpBody;

//   bool ok = doHttpRequest(httpObj, httpStatus, httpBody);

//   Serial.print("doHttpRequest ok=");
//   Serial.print(ok);
//   Serial.print(" status=");
//   Serial.println(httpStatus);

//   if (!ok)
//   {
//     publishError(id, httpBody.c_str(), httpStatus);
//     return;
//   }

//   DynamicJsonDocument res(JSON_DOC_SIZE);
//   res["v"] = 1;
//   res["type"] = "res";
//   res["id"] = id;
//   res["from"] = espId;
//   res["ok"] = true;
//   res["status"] = httpStatus;

//   String bodyB64 = b64Encode((const uint8_t *)httpBody.c_str(), httpBody.length());
//   res["body_b64"] = bodyB64;

//   publishJson(res);
// }

// // ---------- MQTT callback: nur queue ----------
// void mqttCallback(char *topic, byte *payload, unsigned int length)
// {
//   String msg;
//   msg.reserve(length + 1);
//   for (unsigned int i = 0; i < length; i++)
//     msg += (char)payload[i];
//   msg.trim();

//   Serial.println("---- mqttCallback ----");
//   Serial.print("Topic: ");
//   Serial.println(topic);
//   Serial.print("Len: ");
//   Serial.println(msg.length());
//   Serial.print("Raw: ");
//   Serial.println(msg);

//   DynamicJsonDocument doc(JSON_DOC_SIZE);
//   DeserializationError err = deserializeJson(doc, msg);
//   if (err)
//   {
//     Serial.print("JSON parse failed: ");
//     Serial.println(err.c_str());
//     return;
//   }

//   const char *type = doc["type"] | "";
//   const char *from = doc["from"] | "";
//   const char *id = doc["id"] | "";

//   Serial.print("Parsed type=");
//   Serial.print(type);
//   Serial.print(" from=");
//   Serial.print(from);
//   Serial.print(" id=");
//   Serial.println(id);

//   if (strcmp(type, "req") != 0)
//   {
//     Serial.println("Ignore: not a req");
//     return;
//   }

//   if (from[0] != '\0' && espId == String(from))
//   {
//     Serial.println("Ignore: from==espId");
//     return;
//   }

//   pendingMsg = msg;
//   hasPending = true;
//   Serial.println("Queued request for loop()");
// }

// // ---------- MQTT setup/loop ----------
// void setupMqtt()
// {
//   mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
//   mqttClient.setCallback(mqttCallback);

//   bool resized = mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
//   Serial.print("MQTT buffer resized: ");
//   Serial.println(resized ? "OK" : "FAILED");

//   while (!mqttClient.connected())
//   {
//     Serial.print("Verbinde mit MQTT-Broker ... ");
//     String clientId = "esp32c6-";
//     clientId += WiFi.macAddress();

//     if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD))
//     {
//       Serial.println("verbunden");
//       mqttClient.subscribe(MQTT_TOPIC_SUB);
//       Serial.print("Subscribed auf: ");
//       Serial.println(MQTT_TOPIC_SUB);
//     }
//     else
//     {
//       Serial.print("fehlgeschlagen, rc=");
//       Serial.print(mqttClient.state());
//       Serial.println(" -> neuer Versuch in 5s");
//       delay(5000);
//     }
//   }
// }

// void handleMqtt()
// {
//   if (!mqttClient.connected())
//     setupMqtt();
//   mqttClient.loop();
// }

// void setup()
// {
//   // Serial.begin(115200);

//   // espId = "esp-";
//   // espId += WiFi.macAddress();

//   // WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
//   //              { Serial.printf("WiFi event: %d\n", event); }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

//   // WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
//   //              { Serial.printf("Disconnected, reason=%d\n", info.wifi_sta_disconnected.reason); }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

//   // Serial.println("Connecting...");
//   // WiFi.mode(WIFI_STA);
//   // WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

//   // uint8_t r = WiFi.waitForConnectResult();
//   // Serial.printf("waitForConnectResult=%u status=%d\n", r, (int)WiFi.status());

//   // if (r != WL_CONNECTED)
//   // {
//   //   Serial.println("WLAN NICHT verbunden -> stop");
//   //   Serial.printf("SSID=%s RSSI=%d\n", WIFI_SSID, WiFi.RSSI());
//   //   while (true)
//   //   {
//   //     delay(1000);
//   //   } // oder ESP.restart();
//   // }

//   // Serial.println("WLAN verbunden!");
//   // Serial.println(WiFi.localIP());
//   Serial.begin(115200);

//   pinMode(PIN_BL, OUTPUT);
//   digitalWrite(PIN_BL, HIGH); // Backlight an [page:0]

//   SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS); // SCK, MISO(-1), MOSI, CS

//   gfx->begin();
//   gfx->fillScreen(RGB565_RED); // RGB565_* Farben sind im Arduino_GFX README so verwendet [page:0]
//   gfx->setCursor(10, 10);
//   gfx->setTextColor(RGB565_WHITE);
//   gfx->setTextSize(2);
//   gfx->println("Hello LCD");

//   WiFi.mode(WIFI_STA);

//   WiFiManager wm;
//   // optional: wm.setConfigPortalTimeout(180); // Portal nach 3 min zu
//   wm.resetSettings();
//   bool ok = wm.autoConnect("SmartVita Zentrale Setup"); // AP + PW (>=8 Zeichen)
//   if (!ok)
//   {
//     Serial.println("Provisioning fehlgeschlagen");
//     ESP.restart();
//   }

//   Serial.println("WLAN verbunden!");
//   Serial.println(WiFi.localIP());

//   syncTimeOrWarn();

//   // TLS CA fürs MQTTS
//   espClient.setCACert(ROOT_CERT_HIVEMQ);

//   setupMqtt();
// }

// void loop()
// {
//   handleMqtt();

//   if (hasPending)
//   {
//     hasPending = false;

//     Serial.println("loop(): processing pendingMsg");
//     Serial.print("pendingMsg len=");
//     Serial.println(pendingMsg.length());
//     Serial.print("pendingMsg raw=");
//     Serial.println(pendingMsg);

//     DynamicJsonDocument doc(JSON_DOC_SIZE);
//     DeserializationError err = deserializeJson(doc, pendingMsg);

//     Serial.print("loop(): deserialize err=");
//     Serial.println(err.c_str());

//     if (!err)
//     {
//       handleReq(doc);
//     }

//     pendingMsg = ""; // optional: String freigeben
//   }

//   delay(10);
// }
#include <Arduino.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <qrcode.h>

// Waveshare ESP32-C6-LCD-1.47 Pins
static constexpr int PIN_MOSI = 6;
static constexpr int PIN_SCLK = 7;
static constexpr int PIN_CS   = 14;
static constexpr int PIN_DC   = 15;
static constexpr int PIN_RST  = 21;
static constexpr int PIN_BL   = 22;

// HW SPI Bus
Arduino_DataBus *bus = new Arduino_HWSPI(PIN_DC, PIN_CS);

// ST7789 172x320, X-Offset 34 (sonst abgeschnitten/pixelsalat)
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_RST, 0, true,
                                      172, 320,
                                      34, 0, 34, 0);

static void drawQR(const char *text)
{
  // Wenn URL länger ist: version erhöhen (z.B. 8/10) oder ECC senken
  const uint8_t version = 6;

  QRCode qr;
  uint8_t qrData[qrcode_getBufferSize(version)];
  qrcode_initText(&qr, qrData, version, ECC_LOW, text);

  const int border = 4;                      // "quiet zone" in Modulen
  const int modules = qr.size;
  const int total = modules + 2 * border;

  int scale = min(gfx->width() / total, gfx->height() / total);
  if (scale < 1) scale = 1;

  const int x0 = (gfx->width()  - total * scale) / 2;
  const int y0 = (gfx->height() - total * scale) / 2;

  gfx->fillScreen(0xFFFF);                   // weiß

  // QR-Module zeichnen
  for (int y = 0; y < modules; y++) {
    for (int x = 0; x < modules; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        gfx->fillRect(x0 + (x + border) * scale,
                      y0 + (y + border) * scale,
                      scale, scale, 0x0000); // schwarz
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);

  gfx->begin();

  drawQR("https://example.com");
}

void loop() {}
