// Generated from web/index.html by tools/embed_page.py: do not edit.
#pragma once
static const char *BP_PAGE = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Broadcast Control (test)</title>
<style>
:root{--bg:#0e1116;--panel:#171c24;--line:#262d38;--txt:#e6e9ee;--dim:#8a94a3;--acc:#e10600;--ok:#2fbf71;--warn:#f2c230;--blue:#3b82f6}
@media (prefers-color-scheme:light){:root{--bg:#eef0f3;--panel:#fff;--line:#d8dce2;--txt:#14181f;--dim:#5d6675}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--txt);font:14px/1.35 system-ui,Segoe UI,sans-serif}
header{display:flex;flex-wrap:wrap;gap:6px 18px;align-items:center;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--line)}
header b{font-size:16px}.pill{padding:2px 9px;border-radius:99px;background:var(--line);font-size:12px}
.pill.green{background:var(--ok);color:#fff}.pill.yellow,.pill.vsc,.pill.sc,.pill.formation{background:var(--warn);color:#000}.pill.red{background:var(--acc);color:#fff}
#alert{display:none;padding:8px 16px;background:var(--warn);color:#000;font-weight:600}
main{display:grid;grid-template-columns:fit-content(820px) minmax(300px,420px) minmax(280px,380px);justify-content:start;gap:12px;padding:12px 16px}
@media(max-width:1400px){main{grid-template-columns:fit-content(820px) minmax(300px,430px)}#col3{grid-column:2}}
@media(max-width:820px){main{grid-template-columns:1fr}#col3{grid-column:1}}
.col{display:flex;flex-direction:column;gap:12px;min-width:0}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.card h3{margin:0;padding:8px 12px;font-size:12px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.04em;border-bottom:1px solid var(--line)}
table{width:auto;border-collapse:collapse}th{font-weight:500;color:var(--dim);font-size:12px;text-align:left;padding:6px 8px;border-bottom:1px solid var(--line)}
td{padding:5px 8px;border-bottom:1px solid var(--line);white-space:nowrap}th.n,td.n{text-align:right;font-variant-numeric:tabular-nums}
td.drv{max-width:190px;overflow:hidden;text-overflow:ellipsis}
td.int{font-weight:700;font-size:15px;min-width:74px}td.int.close{color:var(--warn)}
th.n,td.n{padding-left:14px}
tr.car{cursor:pointer}tr.car:hover{background:var(--line)}tr.car.on{background:color-mix(in srgb,var(--acc) 22%,transparent);box-shadow:inset 3px 0 var(--acc)}
tr.car.gone td{color:var(--dim)}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;vertical-align:-1px}
.cmp{display:inline-block;width:18px;text-align:center;border-radius:50%;font-size:11px;font-weight:700;border:2px solid var(--dim)}
.cmp.S{border-color:#e10600}.cmp.M{border-color:#f2c230}.cmp.H{border-color:#ddd}.cmp.I{border-color:#2fbf71}.cmp.W{border-color:#3b82f6}
.life{font-size:12px;font-weight:700;font-variant-numeric:tabular-nums;margin-left:4px}
.tag{font-size:10px;font-weight:700;border-radius:4px;padding:1px 5px;margin-left:6px;background:var(--line)}
.tag.y{background:var(--warn);color:#000}.tag.r{background:var(--acc);color:#fff}
.up{color:var(--ok)}.down{color:var(--acc)}
.pad{padding:12px 14px}
h2{margin:0 0 2px;font-size:20px}.dim{color:var(--dim)}
.big{display:flex;gap:18px;align-items:baseline;margin:8px 0}.big .v{font-size:40px;font-weight:700;font-variant-numeric:tabular-nums}
.bar{height:10px;border-radius:5px;background:var(--line);position:relative;margin:4px 0 9px;overflow:hidden}
.bar i{position:absolute;top:0;bottom:0;left:0;background:var(--ok)}.bar.brk i{background:var(--acc)}.bar.st i{background:var(--warn);width:6px}
.tyres{display:grid;grid-template-columns:1fr 1fr;gap:8px}.tyre{background:var(--bg);border-radius:6px;padding:6px 8px;font-variant-numeric:tabular-nums}
.btns{display:flex;gap:6px;flex-wrap:wrap}button{font:inherit;color:var(--txt);background:var(--line);border:1px solid transparent;border-radius:6px;padding:6px 12px;cursor:pointer}
button:hover{border-color:var(--dim)}button.on{background:var(--acc);color:#fff}
#note{min-height:18px;font-size:12px;color:var(--warn);margin-top:6px}
#strip{position:relative;height:62px;margin:10px 14px 14px;border-radius:6px;background:linear-gradient(var(--line),var(--line)) center/100% 4px no-repeat}
#strip .z{position:absolute;top:8px;bottom:8px;background:color-mix(in srgb,var(--warn) 40%,transparent);border:1px solid var(--warn);border-radius:3px}
#strip .sf{position:absolute;top:2px;bottom:2px;width:2px;background:var(--txt);left:0;opacity:.6}
#strip .c{position:absolute;width:16px;height:16px;margin-left:-8px;border-radius:50%;font-size:9px;font-weight:700;color:#fff;text-align:center;line-height:16px;cursor:pointer;border:1px solid rgba(0,0,0,.35)}
#strip .c.on{outline:2px solid var(--acc);z-index:5}
#strip .c.stop{outline:2px solid var(--acc)}
#feed div{padding:6px 12px;border-bottom:1px solid var(--line);display:flex;gap:8px;cursor:pointer}
#feed div:hover{background:var(--line)}#feed .t{color:var(--dim);min-width:52px;font-variant-numeric:tabular-nums;font-size:12px}
#feed .k{font-weight:700;min-width:20px}#feed .new{background:color-mix(in srgb,var(--warn) 18%,transparent)}
#battles div{padding:6px 12px;border-bottom:1px solid var(--line);cursor:pointer}#battles div:hover{background:var(--line)}
.empty{padding:10px 12px;color:var(--dim)}
</style></head><body>
<header><b>Broadcast Control</b> <span class="pill">test</span><span id="trk" class="dim"></span><span id="flag" class="pill"></span>
<span id="dir" class="pill" style="display:none"></span><span id="lap"></span><span id="wx" class="dim"></span><span id="conn" class="pill">connecting</span></header>
<div id="alert"></div>
<main>
 <div class="col">
  <section class="card"><h3>Track (click a car to watch)</h3><div id="strip"></div></section>
  <section class="card"><table><thead><tr><th>P</th><th>Driver</th><th class="n">Int</th><th class="n">Gap</th><th>Tyre</th><th class="n">Last</th><th class="n">Best</th></tr></thead><tbody id="rows"></tbody></table></section>
 </div>
 <aside class="col">
  <section class="card pad"><div class="btns" style="justify-content:space-between;align-items:center"><div><h2 id="wname">-</h2><div class="dim" id="wsub"></div></div>
   <div class="btns"><button id="prev" title="previous car (arrow up)">&#9664;</button><button id="next" title="next car (arrow down)">&#9654;</button></div></div>
   <div class="big"><span class="v" id="spd">0</span><span class="dim">km/h</span><span class="v" id="gear" style="font-size:28px">N</span><span class="dim" id="rpm"></span></div>
   <div class="dim">Throttle</div><div class="bar"><i id="thr"></i></div>
   <div class="dim">Brake</div><div class="bar brk"><i id="brk"></i></div>
   <div class="dim">Steering</div><div class="bar st"><i id="str"></i></div>
   <div class="tyres" id="tyres"></div>
   <p class="dim" id="fuel"></p></section>
  <section class="card pad"><div class="dim" style="margin-bottom:6px">Camera</div>
   <div class="btns"><button id="c1">TV pod</button><button id="c2">Broadcast</button></div><div id="note"></div></section>
  <section class="card"><h3>Race feed</h3><div id="feed"></div></section>
 </aside>
 <aside class="col" id="col3">
  <section class="card"><h3>Battles (closing in)</h3><div id="battles"></div></section>
 </aside>
</main>
<script>
const T="@@TOKEN@@";let S=null,sel=-1,note="",pend=null;
const $=id=>document.getElementById(id),fmt=t=>t>0?(Math.floor(t/60)+":"+(t%60).toFixed(3).padStart(6,"0")):"-";
const gp=g=>g>0?"+"+g.toFixed(1):"";
const col=t=>"hsl("+(t*47%360)+",70%,50%)";
const hist={};   // car -> [{t,int}] the last ~14 s, for the closing arrows
const nameOf=i=>{const c=S&&S.cars.find(x=>x.i===i);return c?c.name:"Car "+i};
const posOf=i=>{const c=S&&S.cars.find(x=>x.i===i);return c&&c.pos?"P"+c.pos:""};
function zoneRanges(z){const r=[];if(!z||!z.length)return r;for(let k=0;k<z.length;k++){const a=z[k].s,b=k+1<z.length?z[k+1].s:1;r.push({a,b,f:z[k].f})}return r}
function inYellow(d,rs){return rs.some(r=>r.f===3&&d>=r.a&&d<r.b)}
function trend(c){const h=hist[c.i];if(!h||h.length<2||!(c.int>0))return 0;const now=h[h.length-1];let old=null;for(const x of h)if(now.t-x.t>=5){old=x}
 if(!old||!(old.int>0))return 0;const r=(old.int-now.int)/(now.t-old.t);return r}   // s per s, positive = closing
const EV={OVTK:["#2fbf71","▲",e=>nameOf(e.a)+" passed "+nameOf(e.b)],COLL:["#e10600","✖",e=>"Contact: "+nameOf(e.a)+" and "+nameOf(e.b)],
 RTMT:["#e10600","✖",e=>nameOf(e.a)+" retired"],FTLP:["#a855f7","★",e=>"Fastest lap: "+nameOf(e.a)],
 SCAR:["#f2c230","⚑",e=>["","Safety car","Virtual safety car","Formation lap"][e.a]+" "+["deployed","returning to pits","has returned","race resumes"][e.b]||"Safety car"],
 FLBK:["#8a94a3","↺",e=>"Flashback"],CHQF:["#ddd","⚑",e=>"Chequered flag"],LGOT:["#2fbf71","●",e=>"Lights out"],
 PENA:["#f2c230","!",e=>"Penalty: "+nameOf(e.a)],DTSV:["#f2c230","!",e=>"Drive-through served: "+nameOf(e.a)],SGSV:["#f2c230","!",e=>"Stop-go served: "+nameOf(e.a)],
 PITI:["#8a94a3","P",e=>nameOf(e.a)+" pits"],PITO:["#8a94a3","P",e=>nameOf(e.a)+" leaves the pits"],
 STOP:["#e10600","!",e=>nameOf(e.a)+" slow or stopped on track (spin?)"],YELL:["#f2c230","⚑",e=>"Yellow flag at "+e.b+"% of the lap"],CLR:["#2fbf71","⚑",e=>"Yellow cleared at "+e.b+"% of the lap"]};
const life=c=>{const w=c.tyre_wear;if(!w||w.length<4)return"";const avg=(w[0]+w[1]+w[2]+w[3])/4,l=Math.max(0,Math.min(100,Math.round(100-avg)));
 const col=l>60?"var(--ok)":l>30?"var(--warn)":"var(--acc)";return "<span class='life' style='color:"+col+"' title='tyre life (average of the four tyres): FL "+(100-w[0]).toFixed(0)+"% FR "+(100-w[1]).toFixed(0)+"% RL "+(100-w[2]).toFixed(0)+"% RR "+(100-w[3]).toFixed(0)+"%'>"+l+"%</span>"};
const ago=s=>s<60?s+" s":Math.floor(s/60)+" min";
function render(s){
 if(s.ready===false){$("conn").textContent="no data";$("conn").className="pill";$("rows").innerHTML="";$("strip").innerHTML="";$("wname").textContent="No data yet";$("wsub").textContent=s.reason||"start a race in the game";return}
 S=s;const se=s.session;$("trk").textContent=typeof se.track==="number"?"Track "+se.track:se.track;$("flag").textContent=se.flag.toUpperCase();$("flag").className="pill "+se.flag;
 $("lap").textContent="Lap "+se.lap+"/"+se.laps;$("wx").textContent=(typeof se.weather==="number"?"weather "+se.weather:se.weather)+" · track "+se.track_temp+"° air "+se.air_temp+"°";
 $("conn").textContent="live ("+(s.src||"")+")";$("conn").className="pill green";
 const w=s.watch;sel=w.active?w.car:-1;
 const dp=s.dir;const de=$("dir");if(dp===undefined||dp<0)de.style.display="none";else{de.style.display="";de.className="pill"+(dp>0?" yellow":" green");de.style.cursor=dp>0?"pointer":"default";de.textContent=dp>0?"Auto director paused "+dp+" s · click to resume":"Auto director on"}
 if(pend){if(w.car===pend.car)pend=null;else if(Date.now()-pend.t>4600){pend=null;$("note").textContent="The game did not switch to that car (it may not be watchable right now)."}}
 const shown=pend?pend.car:sel;
 const now=Date.now()/1000;
 for(const c of s.cars){const h=hist[c.i]||(hist[c.i]=[]);h.push({t:now,int:c.int});while(h.length&&now-h[0].t>14)h.shift()}
 const zr=zoneRanges(s.zones),yz=zr.filter(r=>r.f===3);
 // alert bar: yellow flags and safety car
 let al="";if(se.flag==="sc")al="SAFETY CAR";else if(se.flag==="vsc")al="VIRTUAL SAFETY CAR";else if(yz.length)al="YELLOW FLAG: "+yz.map(r=>Math.round(r.a*100)+"–"+Math.round(r.b*100)+"% of the lap").join(", ")+" · cars in it: "+(s.cars.filter(c=>c.pos&&inYellow(c.d,zr)).map(c=>c.name).join(", ")||"none");
 $("alert").style.display=al?"block":"none";$("alert").textContent=al;
 // track strip
 let h="<div class='sf' title='start/finish'></div>";
 for(const r of yz)h+="<div class='z' style='left:"+r.a*100+"%;width:"+(r.b-r.a)*100+"%' title='yellow flag'></div>";
 const stopped=new Set(s.events.filter(e=>e.k==="STOP"&&e.ago<20).map(e=>e.a));
 s.cars.forEach((c,k)=>{if(!c.pos)return;h+="<div class='c"+(c.i===shown?" on":"")+(stopped.has(c.i)?" stop":"")+"' data-i='"+c.i+"' style='left:"+Math.max(0,Math.min(1,c.d))*100+"%;top:"+(6+(k%3)*15)+"px;background:"+col(c.team)+"' title='"+c.name+"'>"+c.pos+"</div>"});
 $("strip").innerHTML=h;
 // timing table
 $("rows").innerHTML=s.cars.map(c=>{const r=trend(c);const arrow=c.pos>1&&c.int>0&&c.int<3?(r*10>0.15?" <span class='up' title='closing in'>▲</span>":r*10<-0.15?" <span class='down' title='dropping back'>▼</span>":""):"";
  const y=c.pos&&inYellow(c.d,zr)?"<span class='tag y' title='in a yellow flag zone'>⚑</span>":"";
  const tg=(c.out?"<span class='tag r'>OUT</span>":"")+(c.pit?"<span class='tag'>PIT</span>":"")+y+(stopped.has(c.i)?"<span class='tag r'>STOPPED</span>":"");
  return `<tr class="car ${c.i===shown?"on":""} ${c.pos?"":"gone"}" data-i="${c.i}"><td>${c.pos||"-"}</td><td class="drv"><span class="dot" style="background:${col(c.team)}"></span>${c.name}${tg}</td><td class="n int${c.int>0&&c.int<1?" close":""}">${gp(c.int)}${arrow}</td><td class="n">${c.pos===1?"Leader":gp(c.gap)}</td><td><span class="cmp ${c.compound||""}">${c.compound||"?"}</span> ${life(c)}</td><td class="n">${fmt(c.last)}</td><td class="n">${fmt(c.best)}</td></tr>`}).join("");
 // battles
 const bt=s.cars.filter(c=>c.pos>1&&c.int>0&&c.int<1.5&&!c.pit&&!c.out).sort((a,b)=>a.int-b.int).slice(0,6);
 $("battles").innerHTML=bt.length?bt.map(c=>{const r=trend(c),lt=c.best>0?c.best:(c.last>0?c.last:90);const ah=s.cars.find(x=>x.pos===c.pos-1);
  let txt=c.name+" is "+c.int.toFixed(1)+" s behind "+(ah?ah.name:"P"+(c.pos-1));
  if(r*10>0.15){const gain=r*lt;txt+=" · closing "+gain.toFixed(2)+" s/lap";const laps=gain>0.02?c.int/gain:99;if(laps<=6)txt+=" · could pass in ~"+Math.max(1,Math.round(laps))+" lap"+(Math.round(laps)>1?"s":"")}
  else if(r*10<-0.15)txt+=" · falling back";
  if(c.int<1)txt+=" · in overtake range (under 1 s)";
  return "<div data-i='"+c.i+"'>"+txt+"</div>"}).join(""):"<div class='empty'>No close fights right now</div>";
 // feed
 $("feed").innerHTML=s.events.length?s.events.map(e=>{const m=EV[e.k];if(!m)return"";if(e.k==="OVTK"){const xa=s.cars.find(x=>x.i===e.a),xb=s.cars.find(x=>x.i===e.b);if((xa&&(xa.pit||!xa.pos||xa.out))||(xb&&(xb.pit||!xb.pos||xb.out)))return""}return "<div class='"+(e.ago<6?"new":"")+"' data-i='"+(e.k==="YELL"||e.k==="CLR"?-1:e.a)+"'><span class='t'>"+ago(e.ago)+"</span><span class='k' style='color:"+m[0]+"'>"+m[1]+"</span><span>"+m[2](e)+"</span></div>"}).join(""):"<div class='empty'>Nothing yet</div>";
 // watched car
 $("c1").className=w.camera===1?"on":"";$("c2").className=w.camera===2?"on":"";
 const c=s.cars.find(x=>x.i===shown);
 if(!c){$("wname").textContent="Not watching";$("wsub").textContent=w.active?"":"spectate is off in the game";return}
 $("wname").textContent=c.name+(pend?" …":"");$("wsub").textContent=(c.pos?"P"+c.pos+" · ":"")+"lap "+c.lap+" · sector "+c.sector+" · current "+fmt(c.cur);
 $("spd").textContent=c.speed;$("gear").textContent=c.gear<=0?"N":c.gear;$("rpm").textContent=c.rpm+" rpm";
 $("thr").style.width=c.thr*100+"%";$("brk").style.width=c.brk*100+"%";$("str").style.left=(50+c.steer*50)+"%";
 const n=["FL","FR","RL","RR"];$("tyres").innerHTML=c.tyre_temp.map((t,j)=>`<div class="tyre"><span class="dim">${n[j]}</span> ${t}° <span class="dim">wear ${c.tyre_wear[j]}%</span></div>`).join("");
 $("fuel").textContent="Fuel: "+c.fuel_laps.toFixed(1)+" laps";
}
async function tick(){try{const r=await fetch("/api/state?t="+T,{cache:"no-store"});render(await r.json())}catch(e){$("conn").textContent="no connection";$("conn").className="pill"}}
async function post(u){try{const r=await fetch(u,{method:"POST",headers:{"X-Token":T}});const j=await r.json();note=j.ok?"":(j.error||"failed");if(!j.ok)pend=null}catch(e){note="no connection";pend=null}$("note").textContent=note}
function watch(i){if(i<0)return;pend={car:+i,t:Date.now()};if(S)render(S);post("/api/watch?car="+i)}   // the page switches at once, the game follows a moment later
document.addEventListener("click",e=>{const el=e.target.closest("[data-i]");if(el)watch(el.dataset.i)});
$("dir").onclick=()=>{if(S&&S.dir>0)post("/api/director?mode=resume")};
$("c1").onclick=()=>post("/api/cam?mode=1");$("c2").onclick=()=>post("/api/cam?mode=2");
function step(d){if(!S)return;const o=S.cars.filter(c=>c.pos),k=o.findIndex(c=>c.i===(pend?pend.car:sel));if(o.length)watch(o[(k+d+o.length)%o.length].i)}
$("prev").onclick=()=>step(-1);$("next").onclick=()=>step(1);
addEventListener("keydown",e=>{if(e.key==="ArrowUp"){e.preventDefault();step(-1)}if(e.key==="ArrowDown"){e.preventDefault();step(1)}});
setInterval(tick,150);tick();
</script></body></html>
)HTML";
