#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <DHT.h>
#include "RecentHistory.h"
#include "RestartRecord.h"
#include "HistoryImport.h"
#include "ota_secrets.h"
static_assert(sizeof(OTA_PASSWORD_HASH) == 33, "OTA requires a 32-character password hash");
static_assert(sizeof(HISTORY_AUTH_HASH) == 33, "Run setup_ota.py to configure authenticated history restore");

// ESP-01S DHT11 v1.0 carrier: data is normally wired to GPIO2.
constexpr uint8_t DHT_PIN = 2;
constexpr unsigned long SAMPLE_MS = 5000;
DHT dht(DHT_PIN, DHT11);
ESP8266WebServer server(80);
bool sampleOK = false;
float humidity = NAN, temperature = NAN;
unsigned long sampledAt = 0, disconnectedAt = 0;
bool disconnected = false;
String hostname;
RecentHistory history;
// RTC offsets are 4-byte words. Keep clear of the first 128 bytes used by OTA.
constexpr uint32_t RESTART_RTC_OFFSET = 64;
RestartRecord restartRecord = {};
RestartIntent previousRestartIntent = RestartIntent::None;
bool rtcRecordValid = false, rtcWriteOK = false;
String bootResetReason, bootResetInfo;
uint32_t minFreeHeap = UINT32_MAX, wifiDisconnects = 0;
constexpr uint32_t MIN_FREE_HEAP = 8192;
constexpr uint32_t MIN_FREE_BLOCK = 4096;
HistoryImport historyImport;
bool importAuthenticated = false, importComplete = false;
bool importDoneThisRequest = false;
bool importStarted = false, importMultipleFiles = false;
uint32_t historyRestores = 0, lowMemoryResponses = 0;
char historyOutput[1024];
struct HeapSample { uint32_t uptime, freeHeap, largestBlock; uint8_t fragmentation, wifiStatus; };
constexpr uint8_t HEAP_SAMPLES = 48;
HeapSample heapSamples[HEAP_SAMPLES] = {};
uint8_t heapNext = 0, heapCount = 0;
uint32_t lastHeapSample = 0;
static_assert(sizeof(history) + sizeof(historyImport) + sizeof(heapSamples) + sizeof(historyOutput) <= 10000,
  "History, restore staging and debug buffers must remain bounded");

bool memoryAvailable() {
  if (ESP.getFreeHeap() >= MIN_FREE_HEAP && ESP.getMaxFreeBlockSize() >= MIN_FREE_BLOCK) return true;
  ++lowMemoryResponses;
  server.send(503, "application/json", "{\"error\":\"low_memory\"}");
  return false;
}

void collectHeapSample() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
  if (!heapCount || millis() - lastHeapSample >= 60000) {
    lastHeapSample = millis();
    heapSamples[heapNext] = {lastHeapSample, freeHeap, ESP.getMaxFreeBlockSize(),
      ESP.getHeapFragmentation(), uint8_t(WiFi.status())};
    heapNext = (heapNext + 1) % HEAP_SAMPLES;
    if (heapCount < HEAP_SAMPLES) ++heapCount;
  }
}

void receiveHistory() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (importStarted) { importMultipleFiles = true; return; }
    importStarted = true;
    importAuthenticated = upload.contentLength <= 6000 && server.authenticateDigest("history", HISTORY_AUTH_HASH);
    importComplete = false;
    importDoneThisRequest = false;
    historyImport.begin(millis());
  } else if (upload.status == UPLOAD_FILE_WRITE && importAuthenticated && !importDoneThisRequest) {
    historyImport.feed(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END && importAuthenticated) {
    // Multiple files are rejected by the request completion handler.
    importComplete = !importDoneThisRequest && historyImport.complete();
    importDoneThisRequest = true;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    importComplete = false; importStarted = false; importAuthenticated = false; importMultipleFiles = false;
  }
}

void finishHistoryRestore() {
  const bool complete = importStarted && importAuthenticated && importComplete && !importMultipleFiles;
  importStarted = false; importMultipleFiles = false; importComplete = false;
  if (!server.authenticateDigest("history", HISTORY_AUTH_HASH)) {
    server.requestAuthentication(DIGEST_AUTH, "humidity-history"); return;
  }
  if (!complete) {
    server.send(400, "application/json", "{\"error\":\"invalid_history\"}"); return;
  }
  // Only replace the live buffer after the complete, bounded import is validated.
  history = historyImport.staged();
  ++historyRestores;
  server.send(200, "application/json", "{\"restored\":" + String(history.size()) + "}");
  importComplete = false;
}

void sendHeapHistory() {
  if (!memoryAvailable()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"capacity\":48,\"interval_ms\":60000,\"samples\":[");
  char row[192];
  for (uint8_t i = 0; i < heapCount; ++i) {
    const HeapSample& sample = heapSamples[(heapNext + HEAP_SAMPLES - heapCount + i) % HEAP_SAMPLES];
    snprintf(row, sizeof(row), "%s{\"uptime_ms\":%lu,\"free_heap_bytes\":%lu,\"largest_free_block_bytes\":%lu,\"fragmentation_percent\":%u,\"wifi_status\":%u}",
      i ? "," : "", (unsigned long)sample.uptime, (unsigned long)sample.freeHeap,
      (unsigned long)sample.largestBlock, sample.fragmentation, sample.wifiStatus);
    server.sendContent(row); yield();
  }
  server.sendContent("]}"); server.sendContent("");
}

void markRestart(RestartIntent intent) {
  restartRecord = makeRestartRecord(restartRecord.boots, intent);
  rtcWriteOK = ESP.rtcUserMemoryWrite(RESTART_RTC_OFFSET,
    reinterpret_cast<uint32_t*>(&restartRecord), sizeof(restartRecord));
}

void appendJsonString(String& json, const String& value) {
  json += '"';
  for (unsigned i = 0; i < value.length(); ++i) {
    const uint8_t c = value[i];
    if (c == '"' || c == '\\') { json += '\\'; json += char(c); }
    else if (c < 32) {
      const char hex[] = "0123456789abcdef";
      json += "\\u00"; json += hex[c >> 4]; json += hex[c & 15];
    } else json += char(c);
  }
  json += '"';
}

void sendDiagnostics() {
  collectHeapSample();
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxFreeBlockSize();
  const uint8_t fragmentation = ESP.getHeapFragmentation();
  String json;
  if (!json.reserve(3072)) {
    server.send(503, "application/json", "{\"error\":\"diagnostics_allocation_failed\"}"); return;
  }
  json = "{\"reset_reason\":"; appendJsonString(json, bootResetReason);
  json += ",\"reset_info\":"; appendJsonString(json, bootResetInfo);
  json += ",\"previous_restart_intent\":";
  appendJsonString(json, restartIntentName(previousRestartIntent));
  json += ",\"rtc_record_valid_at_boot\":"; json += rtcRecordValid ? "true" : "false";
  json += ",\"rtc_write_ok\":"; json += rtcWriteOK ? "true" : "false";
  json += ",\"boot_count_rtc\":" + String(restartRecord.boots);
  json += ",\"uptime_ms\":" + String(millis());
  json += ",\"free_heap_bytes\":" + String(freeHeap);
  json += ",\"min_observed_free_heap_bytes\":" + String(minFreeHeap);
  json += ",\"largest_free_block_bytes\":" + String(largestBlock);
  json += ",\"heap_fragmentation_percent\":" + String(fragmentation);
  json += ",\"wifi_disconnects_observed\":" + String(wifiDisconnects);
  json += ",\"wifi_status\":" + String(unsigned(WiFi.status()));
  json += ",\"wifi_rssi_dbm\":" + String(WiFi.RSSI());
  json += ",\"wifi_sleep_mode\":" + String(unsigned(WiFi.getSleepMode())) + "}";
  json.remove(json.length() - 1);
  json += ",\"history_count\":" + String(history.size());
  json += ",\"history_capacity\":" + String(RecentHistory::CAPACITY);
  json += ",\"history_buffer_bytes\":" + String(sizeof(history));
  json += ",\"history_import_buffer_bytes\":" + String(sizeof(historyImport));
  json += ",\"heap_log_capacity\":" + String(HEAP_SAMPLES);
  json += ",\"heap_log_bytes\":" + String(sizeof(heapSamples));
  json += ",\"history_restores\":" + String(historyRestores);
  json += ",\"low_memory_responses\":" + String(lowMemoryResponses);
  json += ",\"free_sketch_space\":" + String(ESP.getFreeSketchSpace());
  json += ",\"free_stack_bytes\":" + String(ESP.getFreeContStack()) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Humidity sensor</title><style>
body{font:18px system-ui;max-width:50rem;margin:3vh auto;padding:1rem;background:#f1f5f5;color:#153536}
main{padding:1.5rem;background:white;border-radius:1rem}h1{font-size:1.5rem}h2{font-size:1.1rem;margin-top:2rem}strong{font-size:3rem}small{color:#496263}svg{width:100%;height:auto;display:block}text{font:13px system-ui;fill:#496263}a{color:#15686c}
</style><main><h1>Humidity sensor</h1><p><strong id="humidity">—</strong></p>
<p id="temperature">Waiting for a reading…</p><p><small id="status" role="status">Connecting…</small></p>
<p><small id="uptime">Uptime unavailable</small></p>
<h2>Humidity history</h2><svg id="humidityChart" viewBox="0 0 640 220" role="img" aria-label="Humidity over the last 24 hours"></svg>
<small id="humidityRange"></small>
<h2>Temperature history</h2><svg id="temperatureChart" viewBox="0 0 640 220" role="img" aria-label="Temperature over the last 24 hours"></svg>
<small id="temperatureRange"></small>
<p><small>Rolling 24 hours · recorded every five minutes · clears when the sensor restarts. Times use your browser's clock.</small></p>
<p><small id="historyStatus" role="status">Loading history…</small></p>
<p><a href="/api/readings">JSON readings</a> · <a href="/api/history">JSON history</a></p></main><script>
let deviceUptime=null,uptimeReceivedAt=0;
function showUptime(){
 if(deviceUptime===null)return;
 const elapsed=performance.now()-uptimeReceivedAt,stale=elapsed>15000;
 const seconds=Math.floor((deviceUptime+(stale?0:elapsed))/1000);
 const days=Math.floor(seconds/86400),hours=Math.floor(seconds/3600)%24;
 const minutes=Math.floor(seconds/60)%60,remainder=seconds%60;
 document.getElementById('uptime').textContent='Uptime '+(days?days+'d ':'')+hours+'h '+minutes+'m '+remainder+'s'+(stale?' (last confirmed)':'');
}
setInterval(showUptime,1000);
function chart(id,rangeId,points,key,unit,color,receivedAt){
 const svg=document.getElementById(id);svg.replaceChildren();
 function node(tag,attrs,text){const e=document.createElementNS('http://www.w3.org/2000/svg',tag);for(const [k,v]of Object.entries(attrs))e.setAttribute(k,v);if(text!==undefined)e.textContent=text;svg.append(e);return e;}
 const values=points.map(p=>p[key]).filter(Number.isFinite);
 if(!values.length){node('text',{x:320,y:105,'text-anchor':'middle'},'No valid readings yet');document.getElementById(rangeId).textContent='';return;}
 const low=Math.min(...values),high=Math.max(...values),pad=Math.max(1,(high-low)*.15);
 const min=key==='humidity_percent'?Math.max(0,low-pad):low-pad;
 const max=key==='humidity_percent'?Math.min(100,high+pad):high+pad;
 const span=Math.max(3600,...points.map(p=>p.age_ms/1000));
 const x=p=>56+(1-p.age_ms/1000/span)*568;
 const y=v=>180-(v-min)/(max-min)*160;
 for(let i=0;i<=4;i++){const v=min+(max-min)*i/4;node('line',{x1:56,x2:624,y1:y(v),y2:y(v),stroke:'#e0e8e8'});node('text',{x:48,y:y(v)+4,'text-anchor':'end'},v.toFixed(1));}
 for(let i=0;i<=3;i++){const age=span*(1-i/3),time=new Date(receivedAt-age*1000);node('text',{x:56+568*i/3,y:205,'text-anchor':i===0?'start':i===3?'end':'middle'},time.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'}));}
 let path='',drawing=false,previous=null;
 for(const p of points){const v=p[key];if(!Number.isFinite(v)){drawing=false;previous=null;continue;}if(previous&&previous.age_ms-p.age_ms>450000)drawing=false;path+=(drawing?'L':'M')+x(p).toFixed(1)+','+y(v).toFixed(1);drawing=true;previous=p;node('circle',{cx:x(p),cy:y(v),r:2,fill:color});}
 node('path',{d:path,fill:'none',stroke:color,'stroke-width':2});
 document.getElementById(rangeId).textContent='Low '+low.toFixed(1)+unit+' · High '+high.toFixed(1)+unit;
}
async function refreshHistory(){try{
 const r=await fetch('/api/history',{cache:'no-store',signal:AbortSignal.timeout(10000)});if(!r.ok)throw Error('History unavailable');
 const d=await r.json(),receivedAt=Date.now();
 chart('humidityChart','humidityRange',d.readings,'humidity_percent','% RH','#147b80',receivedAt);
 chart('temperatureChart','temperatureRange',d.readings,'temperature_c',' °C','#b45936',receivedAt);
 document.getElementById('historyStatus').textContent=d.readings.length+' samples · history checked '+new Date().toLocaleTimeString();
 }catch(e){document.getElementById('historyStatus').textContent='History could not refresh; any chart shown is from the last successful check.';}
 finally{setTimeout(refreshHistory,60000);}}
async function refresh(){try{
 const r=await fetch('/api/readings',{cache:'no-store',signal:AbortSignal.timeout(4000)});
 const d=await r.json();
 if(Number.isFinite(d.uptime_ms)&&d.uptime_ms>=0){deviceUptime=d.uptime_ms;uptimeReceivedAt=performance.now();showUptime();}
 if(!r.ok||!d.ok)throw Error('Sensor read failed. Check the DHT11 connection.');
 document.getElementById('humidity').textContent=d.humidity_percent+'% RH';
 document.getElementById('temperature').textContent=d.temperature_c+' °C';
 document.getElementById('status').textContent='Updated '+new Date().toLocaleTimeString()+' · sample '+Math.round(d.sample_age_ms/1000)+'s old';
 }catch(e){document.getElementById('humidity').textContent='—';document.getElementById('temperature').textContent='Reading unavailable';document.getElementById('status').textContent=e.message;}
 finally{setTimeout(refresh,5000);}}
refresh();refreshHistory();</script></html>)HTML";

void sampleSensor() {
  humidity = dht.readHumidity();
  temperature = dht.readTemperature();
  sampledAt = millis();
  sampleOK = isfinite(humidity) && isfinite(temperature) && humidity >= 0 && humidity <= 100 &&
    temperature >= -50 && temperature <= 100;
  // Failed samples remain gaps in history rather than reusing old readings.
  history.addIfDue(sampledAt, sampleOK ? humidity : NAN, sampleOK ? temperature : NAN);
}

void sendHistory() {
  if (!memoryAvailable()) return;
  const uint32_t now = millis();
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"interval_ms\":300000,\"window_ms\":86400000,\"readings\":[");
  size_t outputUsed = 0;
  bool first = true;
  for (uint16_t i = 0; i < history.size(); ++i) {
    const HistoryReading& point = history.at(i);
    const uint32_t age = now - point.sampledAt;
    if (age > RecentHistory::WINDOW_MS) continue;
    char humidityText[16], temperatureText[16], row[144];
    if (isnan(point.humidity)) strcpy(humidityText, "null");
    else dtostrf(point.humidity, 0, 1, humidityText);
    if (isnan(point.temperature)) strcpy(temperatureText, "null");
    else dtostrf(point.temperature, 0, 1, temperatureText);
    const int length = snprintf(row, sizeof(row), "%s{\"age_ms\":%lu,\"humidity_percent\":%s,\"temperature_c\":%s}",
      first ? "" : ",", (unsigned long)age, humidityText, temperatureText);
    if (length < 0 || size_t(length) >= sizeof(row)) break;
    first = false;
    if (outputUsed + size_t(length) > sizeof(historyOutput)) {
      server.sendContent(historyOutput, outputUsed); outputUsed = 0; yield();
    }
    memcpy(historyOutput + outputUsed, row, length); outputUsed += length;
  }
  if (outputUsed) server.sendContent(historyOutput, outputUsed);
  server.sendContent("]}");
  server.sendContent("");
}

void sendReadings() {
  bool fresh = sampleOK && (millis() - sampledAt < 15000);
  String json = "{\"ok\":";
  json += fresh ? "true" : "false";
  json += ",\"humidity_percent\":";
  json += fresh ? String(humidity, 1) : "null";
  json += ",\"temperature_c\":";
  json += fresh ? String(temperature, 1) : "null";
  json += ",\"sample_age_ms\":" + String(millis() - sampledAt);
  json += ",\"uptime_ms\":" + String(millis());
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(fresh ? 200 : 503, "application/json", json);
}

void setup() {
  bootResetReason = ESP.getResetReason();
  bootResetInfo = ESP.getResetInfo();
  rtcRecordValid = ESP.rtcUserMemoryRead(RESTART_RTC_OFFSET,
    reinterpret_cast<uint32_t*>(&restartRecord), sizeof(restartRecord)) && validRestartRecord(restartRecord);
  if (!rtcRecordValid) restartRecord = {};
  previousRestartIntent = beginBootRecord(restartRecord);
  markRestart(RestartIntent::None);
  minFreeHeap = ESP.getFreeHeap();
  Serial.begin(115200);
  Serial.println();
  hostname = "humidity-" + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(hostname.c_str());
  WiFi.setAutoReconnect(true);
  WiFiManager manager;
  manager.setDebugOutput(false); // Keep credentials out of serial logs.
  manager.setConnectTimeout(25);
  manager.setSaveConnectTimeout(25);
  manager.setConfigPortalTimeout(300);
  String apName = "Humidity-Setup-" + String(ESP.getChipId(), HEX);
  Serial.println("If Wi-Fi is unavailable, join " + apName);
  Serial.println("Setup password: humidity-setup. Open http://192.168.4.1");
  if (!manager.autoConnect(apName.c_str(), "humidity-setup")) {
    Serial.println("Setup timed out; restarting.");
    delay(1000);
    markRestart(RestartIntent::SetupTimeout);
    ESP.restart();
    return;
  }
  // Automatic light sleep reduces heat from the adjacent ESP between requests.
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
  dht.begin();
  delay(2000);
  sampleSensor();
  ArduinoOTA.setHostname(hostname.c_str());
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setPasswordHash(OTA_PASSWORD_HASH);
  ArduinoOTA.onStart([]() { Serial.println("Authenticated OTA update starting."); });
  ArduinoOTA.onEnd([]() {
    markRestart(RestartIntent::OtaUpdate);
    Serial.println("OTA complete; restarting.");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA failed (error %u); sensor continues running.\n", unsigned(error));
  });
  ArduinoOTA.begin(); // Starts authenticated OTA and mDNS discovery.
  MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, []() {
    if (!memoryAvailable()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/readings", HTTP_GET, sendReadings);
  server.on("/api/history", HTTP_GET, sendHistory);
  server.on("/api/diagnostics", HTTP_GET, sendDiagnostics);
  server.on("/api/debug/heap", HTTP_GET, sendHeapHistory);
  server.on("/api/history/restore", HTTP_POST, finishHistoryRestore, receiveHistory);
  server.on("/api/device", HTTP_GET, []() {
    String json = "{\"firmware\":\"history-uptime-v6\",\"ota_enabled\":true,\"history_restore\":true,\"hostname\":\"";
    json += hostname + "\",\"sketch_size\":" + String(ESP.getSketchSize());
    json += ",\"free_sketch_space\":" + String(ESP.getFreeSketchSpace());
    json += ",\"wifi_sleep_mode\":" + String(unsigned(WiFi.getSleepMode()));
    json += ",\"uptime_ms\":" + String(millis()) + "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  });
  server.onNotFound([](){server.send(404, "text/plain", "Not found");});
  server.begin();
  Serial.println("Readings: http://" + WiFi.localIP().toString() + "/");
  Serial.println("Also: http://" + hostname + ".local/");
  Serial.println("Password-protected OTA ready on port 8266.");
}

void loop() {
  collectHeapSample();
  ArduinoOTA.handle(); // Includes mDNS updates; sampling pauses during an upload.
  server.handleClient();
  if (millis() - sampledAt >= SAMPLE_MS) sampleSensor();
  if (WiFi.status() == WL_CONNECTED) {
    disconnected = false;
  } else {
    if (!disconnected) { disconnectedAt = millis(); disconnected = true; ++wifiDisconnects; }
    // Reopen the setup portal after a prolonged outage or changed Wi-Fi.
    if (millis() - disconnectedAt >= 180000) {
      markRestart(RestartIntent::WifiTimeout);
      ESP.restart();
    }
  }
  // Allow a full router beacon interval for sleep; requests wait at most one second.
  delay(1000);
}
