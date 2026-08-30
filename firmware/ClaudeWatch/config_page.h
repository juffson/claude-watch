#pragma once
// Web console served at http://claude-watch.local/  (single self-contained page, no external assets)
static const char CONFIG_PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ClaudeWatch 配置</title>
<style>
:root{--bg:#0f0f11;--card:#18181b;--line:#26262b;--txt:#f3efe8;--mut:#8a857d;--acc:#d97757;--ok:#3fb56f;--warn:#4c8dff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.5 -apple-system,"PingFang SC","Helvetica Neue",Arial,sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:24px 16px 48px}h1{font-size:22px;margin:0 0 4px}.sub{color:var(--mut);margin:0 0 20px;font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px 18px 8px;margin-bottom:16px}
.card h2{font-size:13px;letter-spacing:.14em;text-transform:uppercase;color:var(--mut);margin:0 0 12px}
.row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:10px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}.row label{flex:1}.row small{display:block;color:var(--mut);font-size:12px}
input[type=range]{width:200px;accent-color:var(--acc)}input[type=color]{width:44px;height:30px;border:0;background:none;padding:0}
input[type=text],input[type=password],input[type=number]{background:#111114;border:1px solid var(--line);color:var(--txt);border-radius:8px;padding:8px 10px;font-size:14px}
input[type=number]{width:90px}.val{min-width:44px;text-align:right;color:var(--mut);font-variant-numeric:tabular-nums}
.sw{position:relative;width:44px;height:26px;flex:none}.sw input{opacity:0;width:0;height:0}.sw span{position:absolute;inset:0;background:#2a2a2f;border-radius:13px;transition:.2s}
.sw span:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(18px)}
button{background:#222227;color:var(--txt);border:1px solid var(--line);border-radius:10px;padding:8px 14px;font-size:14px;cursor:pointer}button:hover{border-color:var(--acc)}
button.pri{background:var(--acc);border-color:var(--acc);color:#111}.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px;vertical-align:middle;background:#3a3a3f}
.st{display:grid;grid-template-columns:1fr 1fr;gap:6px 16px;font-size:14px}.st b{color:var(--mut);font-weight:500}code{background:#111114;padding:2px 6px;border-radius:6px;font-size:13px}
#toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:#222;border:1px solid var(--line);padding:8px 14px;border-radius:10px;opacity:0;transition:.3s;pointer-events:none}
</style></head><body><div class="wrap">
<h1>ClaudeWatch</h1><p class="sub">ESP32-S3-Touch-AMOLED-1.75C · 修改即时生效并保存在板子上</p>

<div class="card"><h2>Claude 连接</h2>
<div class="st" id="claude">
<b>状态</b><span><i class="dot" id="cdot"></i><span id="cstate">–</span></span>
<b>工具 / 项目</b><span id="ctool">–</span>
<b>最后消息</b><span id="cage">–</span>
<b>会话数</b><span id="csess">–</span>
<b>最新输出</b><span id="cout" style="grid-column:span 1;white-space:pre-wrap;word-break:break-all;color:var(--txt)">–</span></div>
<div class="row"><label>Mac 端 hook<small>Claude Code 通过 hook 推送状态到 <code id="mdns">claude-watch.local</code>；在项目目录执行 <code>python3 host/install_hooks.py</code></small></label><button onclick="post('/api/beep')">测试提示音</button></div>
<div class="row"><label>WAITING / ERROR 时自动跳到 Claude 页</label><label class="sw"><input type="checkbox" data-k="autoJump"><span></span></label></div>
<div class="row"><label>空闲多久后自动回表盘<small>秒</small></label><input type="number" min="5" max="3600" data-k="autoReturnSec"></div>
<div class="row"><label>多久没消息判定离线<small>分钟</small></label><input type="number" min="1" max="1440" data-k="offlineMin"></div>
</div>

<div class="card"><h2>显示</h2>
<div class="row"><label>亮度</label><input type="range" min="10" max="255" data-k="brightness"><span class="val" id="v-brightness"></span></div>
<div class="row"><label>夜间自动调暗<small>23:00–07:00</small></label><label class="sw"><input type="checkbox" data-k="autoDim"><span></span></label></div>
<div class="row"><label>表盘背景色</label><input type="color" data-k="bgColor"></div>
<div class="row"><label>主题色<small>秒刻度等</small></label><input type="color" data-k="accentColor"></div>
<div class="row"><label>显示秒刻度</label><label class="sw"><input type="checkbox" data-k="showTicks"><span></span></label></div>
<div class="row"><label>24 小时制</label><label class="sw"><input type="checkbox" data-k="hour24"><span></span></label></div>
<div class="row"><label>自动熄屏<small>无操作多久后关闭屏幕；触屏/按键或 Claude 需要你时点亮</small></label><select data-k="screenOffSec" style="background:#111114;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px"><option value="0">从不</option><option value="15">15 秒</option><option value="30">30 秒</option><option value="60">1 分钟</option><option value="120">2 分钟</option><option value="300">5 分钟</option><option value="900">15 分钟</option><option value="1800">30 分钟</option></select></div>
<div class="row"><label>接 USB 时保持常亮<small>插着 USB 就忽略上面两个超时，拔掉用电池时才生效</small></label><label class="sw"><input type="checkbox" data-k="usbAlwaysOn"><span></span></label></div>
<div class="row"><label>自动深睡<small>无操作多久后深度睡眠（时钟保持，状态推送暂停）；BOOT 键或触屏唤醒</small></label><select data-k="sleepSec" style="background:#111114;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px"><option value="0">从不</option><option value="300">5 分钟</option><option value="600">10 分钟</option><option value="1800">30 分钟</option><option value="3600">1 小时</option><option value="7200">2 小时</option><option value="14400">4 小时</option></select></div>
</div>

<div class="card"><h2>图片 · 表盘背景与相册</h2>
<div class="row"><label>上传图片<small>自动裁切为 466×466 圆屏尺寸，每张占 424 KB；空间 <span id="imgspace">–</span></small></label><input type="file" id="imgfile" accept="image/*" multiple style="max-width:220px"></div>
<div id="imglist" style="display:flex;flex-wrap:wrap;gap:12px;padding:10px 0"></div>
<div class="row"><label>表盘背景</label><select data-k="wallMode" style="background:#111114;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:8px"><option value="0">关闭（纯色）</option><option value="1">固定一张</option><option value="2">定时轮换</option></select></div>
<div class="row"><label>轮换间隔<small>分钟</small></label><input type="number" min="1" max="1440" data-k="wallRotateMin"></div>
<div class="row"><label>背景压暗<small>越高时间越清楚</small></label><input type="range" min="0" max="90" data-k="wallDim"><span class="val" id="v-wallDim"></span></div>
<div class="row"><label>相册自动翻页<small>秒，0 = 只手动点屏切换</small></label><input type="number" min="0" max="3600" data-k="galleryIntervalSec"></div>
</div>

<div class="card"><h2>提醒</h2>
<div class="row"><label>提示音<small>Claude 等你时响两声，出错响三声（需接喇叭）</small></label><label class="sw"><input type="checkbox" data-k="beep"><span></span></label></div>
<div class="row"><label>音量</label><input type="range" min="0" max="100" data-k="volume"><span class="val" id="v-volume"></span></div>
</div>

<div class="card"><h2>Wi-Fi</h2>
<div class="row"><label>当前<small id="wifi">–</small></label></div>
<div class="row"><label>切换网络<small>保存后板子会重连；网页可能短暂失联</small></label>
<input type="text" id="ssid" placeholder="SSID"><input type="password" id="pass" placeholder="密码"><button class="pri" onclick="wifi()">连接</button></div>
</div>

<div class="card"><h2>系统</h2>
<div class="st"><b>时间</b><span id="time">–</span><b>运行</b><span id="uptime">–</span><b>内存</b><span id="heap">–</span><b>音频</b><span id="audio">–</span><b>语音服务器</b><span id="voice">–</span></div>
<div class="row"><label>对时<small>把这台电脑的时间发给板子（联网时会自动 NTP）</small></label><button onclick="post('/time?epoch='+Math.floor(Date.now()/1000))">同步时间</button></div>
<div class="row"><label>重启</label><button onclick="if(confirm('重启板子？'))post('/api/reboot')">重启</button></div>
</div>
</div><div id="toast"></div>
<script>
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
let t;function toast(m){const e=$('#toast');e.textContent=m;e.style.opacity=1;clearTimeout(t);t=setTimeout(()=>e.style.opacity=0,1500)}
async function post(u,b){try{const r=await fetch(u,{method:'POST',body:b||''});toast(r.ok?'已保存':'失败 '+r.status);return r.ok}catch(e){toast('板子不在线');return false}}
let busyUntil=0;function fill(cfg){if(Date.now()<busyUntil)return;for(const el of $$('[data-k]')){const k=el.dataset.k,v=cfg[k];if(v===undefined||el===document.activeElement)continue;
 if(el.type==='checkbox')el.checked=!!v;else el.value=v;const s=$('#v-'+k);if(s)s.textContent=el.type==='range'&&k==='brightness'?Math.round(v*100/255)+'%':(el.type==='range'?v+'%':'')}}
const names={offline:'离线',idle:'空闲',working:'工作中',waiting:'等待你',error:'错误'},cols={offline:'#3a3a3f',idle:'#3fb56f',working:'#d97757',waiting:'#4c8dff',error:'#e5484d'};
function fmt(s){return s<60?s+' 秒':s<3600?Math.floor(s/60)+' 分':Math.floor(s/3600)+' 小时 '+Math.floor(s%3600/60)+' 分'}
async function load(){try{const d=await (await fetch('/api/config')).json();curWall=d.settings.wallName;fill(d.settings);const st=d.status,i=d.info;
 $('#cstate').textContent=names[st.state]||st.state;$('#cdot').style.background=cols[st.state]||'#3a3a3f';
 $('#ctool').textContent=(st.tool||'–')+(st.project?' / '+st.project:'');$('#cage').textContent=st.seq?fmt(st.age_s)+'前':'从未收到';$('#csess').textContent=st.sessions;$('#cout').textContent=st.output||'–';
 $('#wifi').textContent=i.connected?i.ssid+' · '+i.ip:'未连接 ('+(i.ssid||'未配置')+')';$('#mdns').textContent=i.mdns;
 $('#time').textContent=i.time;$('#uptime').textContent=fmt(i.uptime_s);$('#heap').textContent=Math.round(i.heap/1024)+' KB 可用';$('#audio').textContent=(i.audio?'ES8311 就绪':'未检测到')+(i.mic?' · 麦克风就绪':' · 无麦克风');$('#voice').textContent=i.voiceServer||'未连接（在 Mac 上运行 host/voice_server.py）'}catch(e){}}
let deb={};for(const el of $$('[data-k]')){el.addEventListener(el.type==='range'?'input':'change',()=>{busyUntil=Date.now()+2500;const k=el.dataset.k;
 let v=el.type==='checkbox'?el.checked:(el.type==='number'||el.type==='range'||el.tagName==='SELECT')?+el.value:el.value;const s=$('#v-'+k);if(s)s.textContent=k==='brightness'?Math.round(v*100/255)+'%':v+'%';
 clearTimeout(deb[k]);deb[k]=setTimeout(()=>post('/api/config',JSON.stringify({[k]:v})),el.type==='range'?150:0)})}
async function wifi(){const s=$('#ssid').value.trim();if(!s)return toast('请输入 SSID');await post('/api/wifi',JSON.stringify({ssid:s,pass:$('#pass').value}));setTimeout(load,8000)}
load();setInterval(load,3000);

// ---- images ----
const IMGW=466;
async function encodeImage(file){const bmp=await createImageBitmap(file);const c=document.createElement('canvas');c.width=c.height=IMGW;const x=c.getContext('2d');
 const sc=Math.max(IMGW/bmp.width,IMGW/bmp.height),w=bmp.width*sc,h=bmp.height*sc;x.fillStyle='#000';x.fillRect(0,0,IMGW,IMGW);x.drawImage(bmp,(IMGW-w)/2,(IMGW-h)/2,w,h);
 const d=x.getImageData(0,0,IMGW,IMGW).data,out=new Uint8Array(IMGW*IMGW*2);for(let i=0,j=0;i<d.length;i+=4,j+=2){const v=((d[i]>>3)<<11)|((d[i+1]>>2)<<5)|(d[i+2]>>3);out[j]=v>>8;out[j+1]=v&255}return out}
function safeName(n){n=n.replace(/\.[^.]+$/,'').replace(/[^A-Za-z0-9_-]/g,'').slice(0,20);return (n||('img'+Date.now()%100000))+'.bin'}
$('#imgfile').addEventListener('change',async e=>{for(const f of e.target.files){toast('处理 '+f.name+'…');try{const data=await encodeImage(f);const fd=new FormData();fd.append('file',new Blob([data]),safeName(f.name));
 const r=await fetch('/api/img',{method:'POST',body:fd});toast(r.ok?'已上传 '+f.name:'上传失败 '+r.status)}catch(err){toast('失败: '+err)}}e.target.value='';loadImages()});
async function thumb(name,canvas){try{const buf=new Uint8Array(await (await fetch('/api/img/raw?name='+name)).arrayBuffer());const c=document.createElement('canvas');c.width=c.height=IMGW;const x=c.getContext('2d');
 const id=x.createImageData(IMGW,IMGW),d=id.data;for(let i=0,j=0;j<buf.length;i+=4,j+=2){const v=(buf[j]<<8)|buf[j+1];d[i]=(v>>11)<<3;d[i+1]=((v>>5)&63)<<2;d[i+2]=(v&31)<<3;d[i+3]=255}x.putImageData(id,0,0);
 canvas.getContext('2d').drawImage(c,0,0,96,96)}catch(e){}}
let curWall='';
async function loadImages(){try{const d=await (await fetch('/api/img')).json();$('#imgspace').textContent=Math.round(d.free/1048576*10)/10+' MB 可用 / 还能放 '+Math.floor(d.free/d.slotBytes)+' 张';
 const box=$('#imglist');box.innerHTML='';for(const im of d.images){const el=document.createElement('div');el.style.cssText='width:96px;text-align:center;font-size:12px;color:var(--mut)';
 const cv=document.createElement('canvas');cv.width=cv.height=96;cv.style.cssText='border-radius:50%;display:block;margin:0 auto 4px;background:#000;border:2px solid '+(im.name===curWall?'var(--acc)':'transparent');el.appendChild(cv);thumb(im.name,cv);
 const nm=document.createElement('div');nm.textContent=im.name.replace(/\.bin$/,'');nm.style.cssText='overflow:hidden;text-overflow:ellipsis;white-space:nowrap';el.appendChild(nm);
 const b1=document.createElement('button');b1.textContent='设为背景';b1.style.cssText='font-size:11px;padding:3px 6px;margin:4px 2px 0';b1.onclick=async()=>{await post('/api/config',JSON.stringify({wallMode:1,wallName:im.name}));load();loadImages()};el.appendChild(b1);
 const b2=document.createElement('button');b2.textContent='删除';b2.style.cssText='font-size:11px;padding:3px 6px;margin:4px 2px 0';b2.onclick=async()=>{if(!confirm('删除 '+im.name+'？'))return;await fetch('/api/img?name='+im.name,{method:'DELETE'});loadImages()};el.appendChild(b2);
 box.appendChild(el)}if(!d.images.length)box.innerHTML='<span style="color:var(--mut);font-size:13px">还没有图片，先在上面选择文件上传</span>'}catch(e){}}
loadImages();
</script></body></html>)HTML";
