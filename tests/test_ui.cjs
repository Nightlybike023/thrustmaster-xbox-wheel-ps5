const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
const root=fs.existsSync(path.join(__dirname,'../payload/web'))?'../payload/web':'../web';
const html=n=>fs.readFileSync(path.join(__dirname,root,n+'.html'),'utf8');
const defaults={btn_xbox:65536,btn_menu:8,btn_view:1048576,btn_a:16384,btn_b:8192,btn_x:32768,btn_y:4096,btn_paddle_r:2048,btn_paddle_l:1024,deadzone:368,invert_pedals:0,steer_min:0,steer_center:32768,steer_max:65535,throttle_min:8,throttle_max:1000,brake_min:8,brake_max:1000,sensitivity:100,curve:0,smoothing_ms:0};
let cfg={...defaults},profiles=Array(8).fill(null),failSave=true,failTelemetry=false;
const tele={connected:1,fresh:1,age_ms:5,raw_steer:32768,raw_throttle:8,raw_brake:8,steer:44,throttle:73,brake:21,gear:3,paddle_l:0,paddle_r:1,btn_a:1,btn_b:0,btn_x:0,btn_y:0,btn_up:0,btn_down:0,btn_left:0,btn_right:0,btn_menu:0,btn_view:0,btn_xbox:0};
const diag={stage:5,usb_found:1,handshake_ok:1,virtual_ready:1,age_ms:5,rate_hz:100,input_packets:500,usb_errors:0,injection_attempts:500,injection_errors:0,last_error:0,last_injection:0};
(async()=>{const browser=await chromium.launch({channel:'chrome',headless:true});try{
 const page=await browser.newPage({viewport:{width:360,height:800}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>d.accept());
 await page.route('http://wheel.test/**',async route=>{
  const url=new URL(route.request().url()),p=new URLSearchParams(route.request().postData()||''),slot=Number(p.get('slot'))-1;
  const fields=Object.fromEntries(Object.keys(defaults).filter(k=>p.has(k)).map(k=>[k,Number(p.get(k))])),reply=json=>route.fulfill({json});
  switch(url.pathname){
   case '/':return route.fulfill({contentType:'text/html',body:html('config')});
   case '/overlay':return route.fulfill({contentType:'text/html',body:html('overlay')});
   case '/api/config':return reply(cfg);
   case '/api/telemetry':return failTelemetry?route.fulfill({status:503,json:{error:'offline'}}):reply(tele);
   case '/api/diagnostics':return reply(diag);
   case '/api/profiles':return reply({profiles});
   case '/api/backup':return reply({format:'ps5-wheel-backup',version:1,config:cfg,profiles});
   case '/api/save':assert.equal(route.request().method(),'POST');if(failSave)return route.fulfill({status:500,json:{error:'Falha de disco'}});cfg={...cfg,...fields};return reply({status:'ok'});
   case '/api/profiles/save':profiles[slot]={slot:slot+1,name:p.get('name'),config:{...cfg,...fields}};return reply({status:'ok'});
   case '/api/profiles/load':cfg={...profiles[slot].config};return reply({status:'ok'});
   case '/api/profiles/delete':profiles[slot]=null;return reply({status:'ok'});
   case '/api/reset':cfg={...defaults};return reply({status:'ok'});
   case '/api/gear/reset':tele.gear=Number(p.get('gear'));return reply({status:'ok'});
   case '/api/import':cfg=Object.fromEntries(Object.keys(defaults).map(k=>[k,Number(p.get('cfg.'+k))]));profiles=profiles.map((_,i)=>{const prefix='p'+(i+1)+'.',name=p.get(prefix+'name');return name?{slot:i+1,name,config:Object.fromEntries(Object.keys(defaults).map(k=>[k,Number(p.get(prefix+k))]))}:null;});return reply({status:'ok'});
   default:throw Error('Unhandled '+url.pathname);
  }
 });
 const idle=()=>page.waitForFunction(()=>!document.getElementById('saveConfig').disabled);
 const tab=n=>page.locator('[data-tab="'+n+'"]').click();
 const shot=async n=>{await page.evaluate(()=>{const t=document.getElementById('toast');if(t)t.style.display='none';});if(process.argv[2])await page.screenshot({path:path.join(process.argv[2],n+'.png'),fullPage:true});};
 await page.goto('http://wheel.test/');await idle();assert.equal(await page.locator('#deadzone').inputValue(),'368');assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
 await page.locator('#saveConfig').click();await idle();assert.match(await page.locator('#toast').textContent(),/Falha de disco/);assert.equal(cfg.deadzone,368);failSave=false;
 await page.waitForFunction(()=>document.getElementById('connectionStatus').textContent==='Volante conectado');await shot('calibration-mobile');await page.locator('#startCalibration').click();
 tele.fresh=0;await page.waitForFunction(()=>document.getElementById('captureCalibration').disabled);tele.fresh=1;await page.waitForFunction(()=>!document.getElementById('captureCalibration').disabled);
 const samples=[['raw_steer',30000],['raw_steer',1000],['raw_steer',60000],['raw_throttle',100],['raw_throttle',900],['raw_brake',50],['raw_brake',850]];
 for(let i=0;i<samples.length;i++){
  const [key,value]=samples[i];tele[key]=value;await page.waitForFunction(([k,v])=>latest&&latest[k]===v,[key,value]);
  if(i===1){tele.raw_steer=30000;await page.waitForFunction(()=>latest.raw_steer===30000);await page.locator('#captureCalibration').click();assert.match(await page.locator('#calTitle').textContent(),/^2 de 7/);tele.raw_steer=value;await page.waitForFunction(()=>latest.raw_steer===1000);}
  await page.locator('#captureCalibration').click();
 }
 assert.equal(cfg.steer_center,32768);await page.locator('#saveConfig').click();await idle();assert.equal(cfg.steer_center,30000);assert.equal(cfg.brake_max,850);
 await tab('response');await page.locator('#sensitivity').fill('125');await page.locator('#curve').selectOption('1');await page.locator('#smoothing_ms').fill('10');await shot('direction-mobile');
 await tab('profiles');await page.locator('#profileSlot').selectOption('8');await page.locator('#profileName').fill('GT7 / Forza a\u00e7\u00e3o');await page.locator('#saveProfile').click();await idle();assert.equal(profiles[7].config.sensitivity,125);assert.equal(cfg.sensitivity,100);
 await page.locator('#loadProfile').click();await idle();assert.equal(cfg.sensitivity,125);await shot('profiles-mobile');await tab('backup');
 const downloaded=page.waitForEvent('download');await page.locator('#exportBackup').click();const download=await downloaded;const saved=JSON.parse(fs.readFileSync(await download.path(),'utf8'));await idle();assert.equal(saved.profiles[7].name,'GT7 / Forza a\u00e7\u00e3o');
 await page.locator('#resetDefaults').click();await idle();assert.equal(cfg.steer_center,32768);assert(profiles[7]);
 const invalidBackup=JSON.parse(JSON.stringify(saved));invalidBackup.profiles[7].name='GT7\n';
 await page.locator('#importFile').setInputFiles({name:'invalid-backup.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(invalidBackup))});
 await page.waitForFunction(()=>document.getElementById('toast').textContent.includes('nome de perfil'));await idle();assert.equal(cfg.steer_center,32768);
 await page.locator('#importFile').setInputFiles({name:'backup.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(saved))});await page.waitForFunction(()=>document.getElementById('toast').textContent.includes('Backup restaurado'));await idle();assert.equal(cfg.steer_center,30000);assert.equal(cfg.sensitivity,125);
 await tab('profiles');await page.locator('#profileSlot').selectOption('8');await page.locator('#deleteProfile').click();await idle();assert.equal(profiles[7],null);
 await tab('diagnostics');await page.waitForFunction(()=>document.getElementById('diagRate').textContent==='100 Hz');await shot('diagnostics-mobile');await page.setViewportSize({width:1000,height:850});await shot('diagnostics-desktop');
 await page.setViewportSize({width:360,height:800});await page.goto('http://wheel.test/overlay');await page.waitForFunction(()=>document.getElementById('throttleVal').textContent==='73%');assert.equal(await page.locator('#gearNum').textContent(),'3');assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await shot('overlay-mobile');
 await page.locator('#controlsToggle').click();assert(await page.locator('#controls').isVisible());assert(await page.locator('#btnA').evaluate(e=>e.classList.contains('active')));await page.locator('#controlsToggle').click();
 await page.locator('#viewMenu summary').click();await page.locator('#zoomSlider').fill('1.2');assert.equal(await page.locator('#zoomVal').textContent(),'120%');await page.evaluate(()=>autoFit());await page.locator('#viewMenu summary').click();
 await page.locator('#focusBtn').click();assert(await page.locator('body').evaluate(e=>e.classList.contains('focus')));assert(!(await page.locator('.toolbar').isVisible()));await shot('overlay-focus-mobile');await page.locator('#exitFocus').click();assert(await page.locator('.toolbar').isVisible());
 await page.setViewportSize({width:1440,height:1000});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await shot('overlay-desktop');await page.setViewportSize({width:320,height:700});await page.waitForFunction(()=>baseWidth===296);await shot('overlay-small-mobile');assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await shot('overlay-small-mobile');await page.setViewportSize({width:360,height:800});
 
 await page.locator('#resetGearValue').selectOption('0');await page.locator('#resetGear').click();await page.waitForFunction(()=>document.getElementById('gearNum').textContent==='N');await page.evaluate(()=>setManualZoom(9));assert.equal(await page.locator('#zoomSlider').inputValue(),'2');await page.evaluate(()=>autoFit());
 tele.fresh=0;await page.waitForFunction(()=>document.getElementById('connectionStatus').textContent==='Sem comandos recentes');assert.equal(await page.locator('#gearNum').textContent(),'\u2014');assert(await page.locator('#resetGear').isDisabled());assert.equal(await page.locator('.led.on').count(),0);assert.equal(await page.locator('.ps-btn.active').count(),0);
 tele.connected=0;await page.waitForFunction(()=>document.getElementById('connectionStatus').textContent==='Volante desconectado');failTelemetry=true;await page.waitForFunction(()=>document.getElementById('connectionStatus').textContent==='Sem conex\u00e3o com o PS5');failTelemetry=false;tele.connected=tele.fresh=1;await page.setViewportSize({width:800,height:360});await page.waitForFunction(()=>document.getElementById('connectionStatus').textContent==='Volante conectado');assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await shot('overlay-landscape');
 assert.deepEqual(errors,[]);console.log('PASS: responsive UI, calibration/stale guard, save errors, profiles CRUD, export/import, defaults, diagnostics, cockpit reset, zoom, disconnect');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exit(1);});
