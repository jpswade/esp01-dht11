#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <DHT.h>
#include "RecentHistory.h"
#include "ota_secrets.h"
static_assert(sizeof(OTA_PASSWORD_HASH) == 33, "OTA requires a 32-character password hash");

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
  sampleOK = !isnan(humidity) && !isnan(temperature);
  // Failed samples remain gaps in history rather than reusing old readings.
  history.addIfDue(sampledAt, sampleOK ? humidity : NAN, sampleOK ? temperature : NAN);
}

void sendHistory() {
  const uint32_t now = millis();
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"interval_ms\":300000,\"window_ms\":86400000,\"readings\":[");
  String chunk;
  chunk.reserve(1800);
  bool first = true;
  for (uint16_t i = 0; i < history.size(); ++i) {
    const HistoryReading& point = history.at(i);
    const uint32_t age = now - point.sampledAt;
    if (age > RecentHistory::WINDOW_MS) continue;
    if (!first) chunk += ',';
    first = false;
    chunk += "{\"age_ms\":" + String(age) + ",\"humidity_percent\":";
    chunk += isnan(point.humidity) ? "null" : String(point.humidity, 1);
    chunk += ",\"temperature_c\":";
    chunk += isnan(point.temperature) ? "null" : String(point.temperature, 1);
    chunk += '}';
    if (chunk.length() > 1500) { server.sendContent(chunk); chunk = ""; yield(); }
  }
  chunk += "]}";
  server.sendContent(chunk);
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
  ArduinoOTA.onEnd([]() { Serial.println("OTA complete; restarting."); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA failed (error %u); sensor continues running.\n", unsigned(error));
  });
  ArduinoOTA.begin(); // Starts authenticated OTA and mDNS discovery.
  MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/readings", HTTP_GET, sendReadings);
  server.on("/api/history", HTTP_GET, sendHistory);
  server.on("/api/device", HTTP_GET, []() {
    String json = "{\"firmware\":\"history-uptime-v4\",\"ota_enabled\":true,\"hostname\":\"";
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
  ArduinoOTA.handle(); // Includes mDNS updates; sampling pauses during an upload.
  server.handleClient();
  if (millis() - sampledAt >= SAMPLE_MS) sampleSensor();
  if (WiFi.status() == WL_CONNECTED) {
    disconnected = false;
  } else {
    if (!disconnected) { disconnectedAt = millis(); disconnected = true; }
    // Reopen the setup portal after a prolonged outage or changed Wi-Fi.
    if (millis() - disconnectedAt >= 180000) ESP.restart();
  }
  // Allow a full router beacon interval for sleep; requests wait at most one second.
  delay(1000);
}
