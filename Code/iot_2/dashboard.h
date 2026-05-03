#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <pgmspace.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="vi"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>UTT - May thi cong</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" crossorigin=""/>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',Roboto,sans-serif;background:#e8e8e8;color:#333}
.wrap{display:flex;height:100vh;padding:10px;gap:10px}
.map-col{flex:1;min-width:0;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 4px rgba(0,0,0,.15);display:flex;flex-direction:column}
.map-hdr{display:flex;justify-content:space-between;align-items:center;padding:8px 14px;font-size:14px;font-weight:600;color:#333;border-bottom:1px solid #e0e0e0}
.map-hdr span{cursor:pointer;font-size:18px;color:#666}
#map{flex:1;min-height:0}
.cards-col{display:flex;flex-direction:column;gap:10px;width:420px}
.c-hours{background:linear-gradient(135deg,#1E88E5,#42A5F5);color:#fff;border-radius:8px;padding:16px 20px;display:flex;align-items:center;gap:14px;box-shadow:0 1px 4px rgba(0,0,0,.15)}
.c-hours .ci{width:52px;height:52px;border:3px solid rgba(255,255,255,.7);border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:24px;flex-shrink:0}
.c-hours .ct{flex:1}.c-hours .ct .t1{font-size:16px;font-weight:700}.c-hours .ct .t2{font-size:11px;opacity:.7;margin-top:2px}
.c-hours .cv{font-size:34px;font-weight:700;white-space:nowrap}
.c-hours .cv small{font-size:20px;font-weight:400}
.cards-row{display:flex;gap:10px;flex:1;min-height:0}
.c-temp{flex:1;background:#fff;border-radius:8px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.15);display:flex;flex-direction:column;align-items:center;justify-content:center}
.c-temp .top{display:flex;align-items:center;gap:10px;margin-bottom:16px}
.c-temp .top .ic{font-size:30px;color:#5C6BC0}
.c-temp .top .lb{font-size:14px;font-weight:600;color:#333}
.c-temp .top .sb{font-size:11px;color:#999;margin-top:1px}
.c-temp .val{font-size:52px;font-weight:300;color:#333}
.c-temp .val small{font-size:28px}
.c-status{flex:1;background:linear-gradient(135deg,#43A047,#66BB6A);color:#fff;border-radius:8px;padding:20px;box-shadow:0 1px 4px rgba(0,0,0,.15);display:flex;flex-direction:column;align-items:center;justify-content:center}
.c-status .st{font-size:13px;font-weight:600;opacity:.9;letter-spacing:.3px}
.c-status .sv{font-size:60px;font-weight:700;line-height:1.1;margin:6px 0}
.c-status .si{font-size:26px;opacity:.7}
.c-status.off{background:linear-gradient(135deg,#E53935,#EF5350)}
@media(max-width:700px){.wrap{flex-direction:column;height:auto}.map-col{min-height:280px}.cards-col{width:100%}.cards-row{flex-direction:column}}
</style></head><body>
<div class="wrap">
<div class="map-col">
<div class="map-hdr">Map<span id="fsBtn" title="Fullscreen">&#x26F6;</span></div>
<div id="map"></div>
</div>
<div class="cards-col">
<div class="c-hours">
<div class="ci">&#128339;</div>
<div class="ct"><div class="t1">tong gio hoat dong</div><div class="t2" id="updTime">Last update just now</div></div>
<div class="cv"><span id="rpm">--</span> <small>min</small></div>
</div>
<div class="cards-row">
<div class="c-temp">
<div class="top"><div class="ic">&#127777;&#65039;</div><div><div class="lb">Temperature</div><div class="sb" id="tmpTime">Last update just now</div></div></div>
<div class="val"><span id="temp">--</span> <small>&#176;C</small></div>
</div>
<div class="c-status" id="eCard">
<div class="st">so may hoat dong</div>
<div class="sv" id="engine">0</div>
<div class="si">&#9741;</div>
</div>
</div>
</div>
</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js" crossorigin=""></script>
<script>
var map,marker,mapOk=false,lastUpd='';
function initMap(lat,lon){try{map=L.map('map').setView([lat,lon],15);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,
attribution:'&copy; <a href="https://openstreetmap.org">OpenStreetMap</a> contributors'}).addTo(map);
marker=L.marker([lat,lon]).addTo(map).bindPopup('esp32');mapOk=true;
setTimeout(function(){map.invalidateSize();},200);}catch(e){}}
function updMap(lat,lon){if(!mapOk){initMap(lat,lon);return;}
marker.setLatLng([lat,lon]);map.panTo([lat,lon]);}
document.getElementById('fsBtn').onclick=function(){var el=document.querySelector('.map-col');
if(el.requestFullscreen)el.requestFullscreen();else if(el.webkitRequestFullscreen)el.webkitRequestFullscreen();
setTimeout(function(){if(map)map.invalidateSize();},300);};
function ts(){var d=new Date();var h=d.getHours(),m=d.getMinutes();
return 'Last update '+(h<10?'0':'')+h+':'+(m<10?'0':'')+m;}
function upd(){fetch('/api/status').then(function(r){return r.json();}).then(function(d){
var now=ts();document.getElementById('updTime').textContent=now;
document.getElementById('tmpTime').textContent=now;
document.getElementById('rpm').textContent=d.rpm;
document.getElementById('temp').textContent=d.temperature.toFixed(1);
var ec=document.getElementById('eCard'),en=document.getElementById('engine');
if(d.engine_on){en.textContent='1';ec.className='c-status';}
else{en.textContent='0';ec.className='c-status';}
if(d.gps_valid&&d.latitude!=0){updMap(d.latitude,d.longitude);}
}).catch(function(e){console.log('err',e);});}
initMap(21.594,105.848);upd();setInterval(upd,3000);
</script></body></html>
)rawliteral";

#endif
