#pragma once
#include <WiFi.h>
#include <WebServer.h>

// Connect directly from a phone or laptop. Change these before uploading.
constexpr char DashboardSsid[] = "E38-Holley-Gateway";
constexpr char DashboardPassword[] = ""; // Set your own password, at least 8 characters.

struct ParameterReading
{
  float value = 0;
  uint32_t updated = 0;
  uint32_t count = 0;
  int result = 0;
  byte length = 0;
  byte data[8]{};
};

struct DashboardState
{
  ParameterReading readings[8];
  bool ready = false;
  uint32_t can2Ok = 0;
  uint32_t can2Pending = 0;
  uint32_t can2NoBuffer = 0;
  uint32_t can2Other = 0;
  uint32_t can1Failures = 0;
  uint32_t can1Missed = 0;
  byte tec = 0;
  byte errors = 0;
};

DashboardState dashboard;
portMUX_TYPE dashboardLock = portMUX_INITIALIZER_UNLOCKED;
WebServer dashboardServer(80);

void recordParameter(unsigned index, float value, const byte *data,
                     byte length, int result)
{
  const uint32_t now = millis();
  portENTER_CRITICAL(&dashboardLock);
  ParameterReading &reading = dashboard.readings[index];
  reading.value = value;
  reading.updated = now;
  ++reading.count;
  reading.result = result;
  reading.length = length;
  memcpy(reading.data, data, length);
  portEXIT_CRITICAL(&dashboardLock);
}

const char DashboardPage[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>E38 / Holley CAN gateway</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#10151d;color:#e7edf5}
body{max-width:1200px;margin:auto;padding:24px}h1{font-size:28px;margin-bottom:8px}
h2{font-size:20px;margin-top:32px}p{color:#aebbc9;line-height:1.5}
.scroll{overflow:auto;background:#19222e;border-radius:12px}
table{border-collapse:collapse;width:100%;white-space:nowrap}
th,td{text-align:left;padding:13px;border-bottom:1px solid #303b49}
th{color:#aebbc9;font-size:13px}td code{font-size:12px}.fresh{color:#75ddba}
.stale{color:#ffc47a}#connection{font-weight:600}#stats{font-family:monospace}
</style>
<h1>E38 / Holley CAN gateway</h1>
<p id="connection">Connecting...</p>
<h2>GM CAN1 to Holley CAN2</h2>
<div class="scroll"><table><thead><tr><th>Parameter</th><th>Value</th><th>CAN IDs</th>
<th>Outgoing bytes</th><th>Age</th><th>Attempts</th><th>Latest TX result</th></tr></thead>
<tbody id="gm"></tbody></table></div>
<h2>Holley CAN2 to GM CAN1</h2>
<div class="scroll"><table><thead><tr><th>Parameter</th><th>Value</th><th>CAN IDs</th>
<th>Bytes</th><th>Age</th><th>Updates</th><th>Latest result</th></tr></thead>
<tbody id="holley"></tbody></table></div>
<p>Age measures the latest received input or transmit attempt. Amber marks values older than
3 seconds. This dashboard timeout is independent of the Holley NODATA timeout.
CAN2 pending means the library stopped waiting while the frame was still pending.
CAN1 queued means TWAI accepted the frame into its transmit queue.</p>
<h2>CAN status</h2><p id="stats">Waiting for status...</p>
<script>
const labels=[
 ['Fuel level','%','0x4D1 → 0x1E202001'],
 ['Reduced power','','0x4D1 → 0x1E20E001'],
 ['A/C request','','0x3D1 → 0x1E206001'],
 ['A/C high-side pressure','psi','0x3F9 → 0x1E20A001'],
 ['Outside air','°F','0x3F1 → 0x1E212001'],
 ['Park input','','0x1E52D000 / mask 0x1FFFF800'],
 ['Gear input','','0x1E4FD000 / mask 0x1FFFF800'],
 ['GM gear output','','0x1F5']];
function row(r,i){
 const [label,unit,ids]=labels[i],seen=r.count>0;
 let value=seen?r.value.toFixed(2)+' '+unit:'Waiting';
 if(seen&&[1,2,5].includes(i))value=r.value?'On':'Off';
 if(seen&&i===7)value=r.value===0?(r.data[0]===15?'Park':'Neutral'):r.value===-1?'Reverse':'Gear '+r.value;
 const bytes=seen?r.data.map(b=>b.toString(16).padStart(2,'0').toUpperCase()).join(' '):'—';
 let result='Received';
 if(i<5)result=r.result===0?'Completed':r.result===7?'Pending at timeout':r.result===6?'No TX buffer':'Error '+r.result;
 if(i===7)result=r.result===0?'Queued':'Queue error '+r.result;
 return '<tr><td>'+label+'</td><td>'+value+'</td><td><code>'+ids+'</code></td><td><code>'+bytes+
 '</code></td><td class="'+(r.age>3000?'stale':'fresh')+'">'+(seen?(r.age/1000).toFixed(1)+' s':'—')+
 '</td><td>'+r.count+'</td><td>'+(seen?result:'—')+'</td></tr>';
}
async function refresh(){
 try{
  const response=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(2500)});
  if(!response.ok)throw Error('HTTP '+response.status);
  const s=await response.json();
  document.getElementById('connection').textContent=s.ready?'Connected · Gateway running':'Connected · CAN initialization failed';
  document.getElementById('gm').innerHTML=s.readings.slice(0,5).map((r,i)=>row(r,i)).join('');
  document.getElementById('holley').innerHTML=s.readings.slice(5).map((r,i)=>row(r,i+5)).join('');
  document.getElementById('stats').textContent='CAN2 completed '+s.ok+' · pending timeouts '+s.pending+
   ' · no buffer '+s.noBuffer+' · other errors '+s.other+' · TEC '+s.tec+' · EFLG 0x'+
   s.errors.toString(16).padStart(2,'0')+' | CAN1 queue failures '+s.can1Failures+' · RX missed '+s.missed;
 }catch(e){
  document.getElementById('connection').textContent='Disconnected · displayed values are frozen';
  document.querySelectorAll('tbody td:nth-child(5)').forEach(e=>{e.className='stale';e.textContent='Offline'});
 }finally{setTimeout(refresh,500)}
}
refresh();
</script></html>
)HTML";

void serveDashboardStatus()
{
  DashboardState snapshot;
  portENTER_CRITICAL(&dashboardLock);
  snapshot = dashboard;
  portEXIT_CRITICAL(&dashboardLock);
  const uint32_t now = millis();
  String json;
  json.reserve(2500);
  json = "{\"ready\":";
  json += snapshot.ready ? "true" : "false";
  json += ",\"ok\":" + String(snapshot.can2Ok);
  json += ",\"pending\":" + String(snapshot.can2Pending);
  json += ",\"noBuffer\":" + String(snapshot.can2NoBuffer);
  json += ",\"other\":" + String(snapshot.can2Other);
  json += ",\"tec\":" + String(snapshot.tec);
  json += ",\"errors\":" + String(snapshot.errors);
  json += ",\"can1Failures\":" + String(snapshot.can1Failures);
  json += ",\"missed\":" + String(snapshot.can1Missed);
  json += ",\"readings\":[";
  for (unsigned i = 0; i < 8; ++i)
  {
    const ParameterReading &r = snapshot.readings[i];
    if (i) json += ',';
    json += "{\"value\":" + String(r.value, 4);
    json += ",\"age\":" + String(uint32_t(now - r.updated));
    json += ",\"count\":" + String(r.count);
    json += ",\"result\":" + String(r.result);
    json += ",\"data\":[";
    for (byte j = 0; j < r.length; ++j)
    {
      if (j) json += ',';
      json += String(r.data[j]);
    }
    json += "]}";
  }
  json += "]}";
  dashboardServer.sendHeader("Cache-Control", "no-store");
  dashboardServer.send(200, "application/json", json);
}

void dashboardTask(void *)
{
  if (strlen(DashboardPassword) < 8)
  {
    Serial.println("Set DashboardPassword in Dashboard.h to at least 8 characters");
    vTaskDelete(nullptr);
    return;
  }
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(DashboardSsid, DashboardPassword))
  {
    Serial.println("Dashboard Wi-Fi initialization failed");
    vTaskDelete(nullptr);
    return;
  }
  Serial.print("Dashboard address: http://");
  Serial.println(WiFi.softAPIP());
  dashboardServer.on("/", HTTP_GET, []() {
    dashboardServer.send_P(200, "text/html", DashboardPage);
  });
  dashboardServer.on("/api/status", HTTP_GET, serveDashboardStatus);
  dashboardServer.begin();
  for (;;)
  {
    dashboardServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5) + 1);
  }
}
