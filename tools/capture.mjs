/* GCS 시뮬레이션을 구동해 문서용 화면 캡처를 만듭니다. */
import { chromium } from 'playwright';
const OUT = '/home/claude/repo/usv/docs/images';
const URL = 'file:///home/claude/repo/usv/gcs/index.html';
const b = await chromium.launch({executablePath:'/opt/pw-browsers/chromium'});
const errs = [];

async function page(w=1560,h=1480,dpr=1){
  const p = await b.newPage({viewport:{width:w,height:h}, deviceScaleFactor:dpr});
  p.on('pageerror', e=>errs.push('PAGEERROR: '+e.message));
  p.on('console', m=>{ if(m.type()==='error') errs.push('CONSOLE: '+m.text()); });
  await p.goto(URL); await p.waitForTimeout(400);
  return p;
}
const shot = (p,name,sel)=> sel ? p.locator(sel).screenshot({path:`${OUT}/${name}.png`})
                                : p.screenshot({path:`${OUT}/${name}.png`});

/* 1) 첫 화면 */
let p = await page();
await shot(p,'gcs-idle');
await p.close();

/* 2) 정상 운항 — 전체 + 부분 */
p = await page();
await p.click('#btnSim');  await p.waitForTimeout(300);
await p.click('#btnArm');  await p.click('#btnRec');
await p.keyboard.down('KeyW'); await p.waitForTimeout(3000);
await p.keyboard.down('KeyD'); await p.waitForTimeout(2300); await p.keyboard.up('KeyD');
await p.waitForTimeout(2200);
await p.keyboard.down('KeyA'); await p.waitForTimeout(2000); await p.keyboard.up('KeyA');
await p.waitForTimeout(3000); await p.keyboard.up('KeyW');
await p.click('#simAuto'); await p.waitForTimeout(6000); await p.click('#simAuto');
await p.waitForTimeout(1500);
await shot(p,'gcs-full');
await shot(p,'tiles','.tiles');
await shot(p,'charts-all','main > .col:nth-child(2) > .charts:nth-child(1)');
await shot(p,'link-quality','main > .col:nth-child(1) > .card:nth-child(3)');
await shot(p,'sim-camera','main > .col:nth-child(1) > .card:nth-child(2)');
await shot(p,'control','main > .col:nth-child(1) > .card:nth-child(4)');
await shot(p,'sim-panel','#simPanel');
await shot(p,'track','main > .col:nth-child(2) > .charts:nth-child(2) > .card:nth-child(1)');
await shot(p,'record','main > .col:nth-child(2) > .charts:nth-child(2) > .card:nth-child(2)');
await shot(p,'telemetry-detail','main > .col:nth-child(2) > .card:last-child');
const st = await p.evaluate(()=>({dist:+simDist.textContent,grade:tGrade.textContent,
  rssi:T.rssi,vbat:T.vbat,rec:recCount.textContent,pts:chAtt.data.length}));
console.log('정상운항:', JSON.stringify(st));
await p.close();

/* 3) 통신 약화 */
p = await page();
await p.click('#btnSim'); await p.waitForTimeout(200);
await p.selectOption('#simScn','weaklink');
await p.evaluate(()=>{ sim.reset(); sim.scn='weaklink'; });
await p.click('#btnArm');
await p.click('#simAuto'); await p.waitForTimeout(9000); await p.click('#simAuto');
await p.waitForTimeout(1500);
await shot(p,'scenario-weaklink');
await shot(p,'tiles-weaklink','.tiles');
await shot(p,'link-quality-weak','main > .col:nth-child(1) > .card:nth-child(3)');
console.log('통신약화:', JSON.stringify(await p.evaluate(()=>({dist:+simDist.textContent,
  rssi:T.rssi,grade:tGrade.textContent,loss:tLoss.textContent,rtt:tRtt.textContent}))));
await p.close();

/* 4) 파고 높음 */
p = await page();
await p.click('#btnSim'); await p.waitForTimeout(200);
await p.selectOption('#simScn','rough');
await p.evaluate(()=>{ sim.reset(); sim.scn='rough'; });
await p.click('#btnArm');
await p.keyboard.down('KeyW'); await p.waitForTimeout(6000);
await p.keyboard.down('KeyD'); await p.waitForTimeout(2500); await p.keyboard.up('KeyD');
await p.waitForTimeout(3000); await p.keyboard.up('KeyW'); await p.waitForTimeout(1200);
await shot(p,'charts-rough','main > .col:nth-child(2) > .charts:nth-child(1)');
await shot(p,'sim-camera-rough','main > .col:nth-child(1) > .card:nth-child(2)');
await p.close();

/* 5) 전복 */
p = await page();
await p.click('#btnSim'); await p.waitForTimeout(200);
await p.selectOption('#simScn','capsize');
await p.evaluate(()=>{ sim.reset(); sim.scn='capsize'; });
await p.click('#btnArm'); await p.evaluate(()=>{ sim.t = 11; });
await p.waitForTimeout(9500);
await shot(p,'scenario-capsize');
await shot(p,'charts-capsize','main > .col:nth-child(2) > .charts:nth-child(1) > .card:nth-child(1)');
await shot(p,'log-capsize','main > .col:nth-child(2) > .charts:nth-child(2) > .card:nth-child(2)');
await shot(p,'telemetry-capsize','main > .col:nth-child(2) > .card:last-child');
console.log('전복:', JSON.stringify(await p.evaluate(()=>({roll:T.roll,arm:T.arm,fs:T.fs}))));
await p.close();

/* 6) 통신 두절 */
p = await page();
await p.click('#btnSim'); await p.waitForTimeout(200);
await p.click('#btnArm');
await p.keyboard.down('KeyW'); await p.waitForTimeout(3500); await p.keyboard.up('KeyW');
await p.selectOption('#simScn','blackout');
await p.evaluate(()=>{ sim.scn='blackout'; });
await p.waitForTimeout(3500);
await shot(p,'scenario-blackout');
await shot(p,'header-blackout','header');
console.log('통신두절:', JSON.stringify(await p.evaluate(()=>({txt:connTxt.textContent,
  stale:document.body.classList.contains('stale')}))));
await p.close();

/* 7) 고해상도(dpr 2) 회귀 확인 — 차트가 비지 않는지 */
p = await page(1500,1000,2);
await p.click('#btnSim'); await p.waitForTimeout(300);
await p.click('#btnArm');
await p.keyboard.down('KeyW'); await p.waitForTimeout(2500); await p.keyboard.up('KeyW');
for(const w of [1300,1500,1100,1500]){ await p.setViewportSize({width:w,height:1000}); await p.waitForTimeout(150); }
await p.waitForTimeout(800);
const hi = await p.evaluate(()=>{
  const ids=['cAtt','cYaw','cAcc','cGz','cSpd','cVolt','cRssi','cMap'];
  let bad=0, sizes=[];
  for(const id of ids){ const c=document.getElementById(id);
    sizes.push(`${c.width}x${c.height}`); if(c.height>2000||c.width>8000) bad++; }
  return {bad, cAtt:sizes[0], cMap:sizes[7]};
});
console.log('dpr2 회귀:', JSON.stringify(hi));
await p.close();

console.log(errs.length ? errs.slice(0,6).join('\n') : '오류 없음');
await b.close();
