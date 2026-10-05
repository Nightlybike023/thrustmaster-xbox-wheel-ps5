const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
const root=fs.existsSync(path.join(__dirname,'../payload/web'))?'../payload/web':'../web';
const html=fs.readFileSync(path.join(__dirname,root,'overlay.html'),'utf8');
(async()=>{
 const browser=await chromium.launch({channel:'chrome',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:360,height:800}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://wheel.test/**',route=>route.fulfill(new URL(route.request().url()).pathname==='/overlay'?{contentType:'text/html',body:html}:{json:{connected:0,fresh:0}}));
  await page.goto('http://wheel.test/overlay');
  assert.equal(await page.evaluate(()=>!!navigator.wakeLock),false,'real HTTP origin must exercise video fallback');
  const button=page.locator('#keepScreenBtn');
  const state=expected=>page.waitForFunction(s=>document.getElementById('keepScreenBtn').dataset.state===s,expected);
  await button.click();await state('video');
  await page.waitForFunction(()=>screenVideo.currentTime>0&&!screenVideo.paused&&screenVideo.videoWidth>0);
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await page.setViewportSize({width:320,height:700});await page.waitForFunction(()=>baseWidth===296);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  await button.click();await state('off');assert(await page.evaluate(()=>screenVideo.paused));
  // Exercise both embedded codecs, including the long MP4 seek workaround.
  await page.evaluate(()=>{screenVideo.src=screenVideoSources.mp4;screenVideo.load();});
  await button.click();await state('video');await page.waitForFunction(()=>screenVideo.currentTime>0&&screenVideo.videoWidth>0);
  const visibility=hidden=>page.evaluate(h=>{Object.defineProperty(document,'hidden',{configurable:true,value:h});document.dispatchEvent(new Event('visibilitychange'));},hidden);
  await visibility(true);await state('paused');assert(await page.evaluate(()=>screenVideo.paused));
  await visibility(false);await state('video');
  await page.evaluate(()=>screenVideo.pause());await state('retry');await button.click();await state('video');
  await button.click();await state('off');
  await page.evaluate(()=>{screenVideo.play=()=>Promise.reject(new DOMException('Blocked','NotAllowedError'));});
  await button.click();await state('retry');
  await page.reload();
  await page.evaluate(()=>{
   window.locks=[];window.requests=0;window.requestMode='ok';
   Object.defineProperty(navigator,'wakeLock',{configurable:true,value:{request:async type=>{
    if(type!=='screen')throw Error('Wrong lock type');window.requests++;
    if(window.requestMode==='reject')throw new DOMException('Denied','NotAllowedError');
    if(window.requestMode==='defer')await new Promise(r=>window.resolveLock=r);
    const lock=new EventTarget();lock.released=false;lock.release=async()=>{lock.released=true;lock.dispatchEvent(new Event('release'));};window.locks.push(lock);return lock;
   }}});
  });
  await button.click();await state('native');assert.equal(await page.evaluate(()=>window.requests),1);
  await visibility(true);await state('paused');assert(await page.evaluate(()=>window.locks[0].released));
  await visibility(false);await state('native');assert.equal(await page.evaluate(()=>window.requests),2);
  await page.evaluate(()=>window.locks.at(-1).release());await state('retry');
  await button.click();await state('native');await button.click();await state('off');
  assert(await page.evaluate(()=>window.locks.every(l=>l.released)));
  await page.evaluate(()=>window.requestMode='defer');await button.click();await state('pending');await button.click();await state('off');
  await page.evaluate(()=>window.resolveLock());await page.waitForFunction(()=>window.locks.every(l=>l.released));await state('off');
  await page.evaluate(()=>window.requestMode='reject');await button.click();await state('retry');
  assert.equal(await page.evaluate(()=>screenVideo),null,'native denial must not silently start media');
  await page.evaluate(()=>window.requestMode='ok');await button.click();await state('native');
  await page.evaluate(()=>dispatchEvent(new Event('pagehide')));await state('paused');assert(await page.evaluate(()=>window.locks.every(l=>l.released)));
  await page.evaluate(()=>dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true})));await state('native');
  await button.click();await state('off');assert.deepEqual(errors,[]);
  console.log('PASS: HTTP silent WebM/MP4 playback, mobile layout, enable/disable, visibility resume, media denial/interruption, native release/denial, cancellation race, page lifecycle');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
