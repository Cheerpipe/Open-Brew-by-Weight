{
  const cupAt = runtimeJs.indexOf('function formatCupState(');
  const format = new Function(runtimeJs.slice(cupAt, runtimeJs.indexOf('\n}', cupAt) + 2) +
    ';return formatCupState;')();
  for (const cupPresence of [{state:'ABSENT',present:false}, {state:'PRESENT',present:true}]) {
    for (const scale of [undefined, {available:false}, {available:true}]) {
      const expected = scale?.available ? (cupPresence.present ? 'Present' : 'Absent') : 'Unknown';
      if (format({scale,cupPresence}) !== expected)
        throw new Error('Disconnected cup status must be Unknown; connected presence must be preserved');
    }
  }
  const required = {cupProtectionEnabled:true, requireCupToStart:true};
  if (format({scale:{available:true}, cupPresence:{present:false}, config:required}) !== 'Absent - cup required' ||
      format({scale:{available:true}, cupPresence:{present:false}, config:{...required, cupProtectionEnabled:false}}) !== 'Absent' ||
      format({scale:{available:true}, cupPresence:{present:false}, config:{...required, brewByWeight:false}}) !== 'Absent')
    throw new Error('An absent cup must say it is required only when cup protection requires it');
}
{
  const helpers = runtimeJs.slice(runtimeJs.indexOf('function axisLabel('),
      runtimeJs.indexOf('async function populateTimezoneOptions(')) + statsChartJs;
  const preset = runtimeJs.slice(runtimeJs.indexOf('function buildRuleChartModel('),
      runtimeJs.indexOf('function positionPresetScrub('));
  // The shot chart renderer lives in the Stats module; its runtime helpers reach it through R.
  const renderer = runtimeJs.slice(runtimeJs.indexOf('function shotRuleModel('),
      runtimeJs.indexOf('function setEmptyState(')) +
    viewJs.stats.slice(viewJs.stats.indexOf('// The value of a time-sorted curve'),
      viewJs.stats.indexOf('let statsPresetFilter'));
  // One combined chart: a weight axis, a flow axis, one time axis and the final-yield overlay.
  const ticks = [], markers = [], axes = [], keys = [{style:{}}, {style:{}}];
  let markup = '', plot = null;
  const dots = [0,1,2,3].map(()=>({attrs:{},style:{setProperty(){}},setAttribute(k,v){this.attrs[k]=v}})),
    cursor = {firstChild:{children:dots}, style:{setProperty(){}}};
  const document = {createElement(){return {style:{},dataset:{},setAttribute(){}}}};
  const tickSink = {replaceChildren(){ticks.length=0}, appendChild(item){ticks.push(item)},
    setAttribute() {}, removeAttribute() {}, dataset: {}};
  const host = {
    get innerHTML(){return markup},
    set innerHTML(value){markup=value;markers.length=0;axes.length=0;
      for(const part of value.split('<div class="shotSparkY').slice(1)){const y=(part.split('</div>')[0].match(/class="shotYTick"/g)||[]).map(()=>({style:{},dataset:{},textContent:'0'}));axes.push({querySelectorAll(){return y},setAttribute(){},dataset:{},style:{}})}
      plot=value.includes('class="shotSparkHost"')?{style:{setProperty(k,v){this[k]=v}},querySelectorAll(){return axes},
        attrs:{},setAttribute(k,v){this.attrs[k]=v},addEventListener(){},
        querySelector(s){return s==='.shotSparkCursor'?cursor:tickSink}}:null},
    hidden:true, replaceChildren(){this.innerHTML=''},
    querySelector(selector){return selector==='.shotSparkHost'?plot:selector==='.shotDropOverlay'?{appendChild(item){markers.push(item)}}:{}},
    querySelectorAll(){return keys},
    append(){},
  };
  const stretched = (s) => s.split('shotSparkCursor')[0];
  const fills = () => (markup.match(/fill="url\(#sc/g)||[]).length;
  const render = new Function('document', 'ResizeObserver', '$', 'requestAnimationFrame',
      'const presetState={};' + helpers + preset + renderer +
      runtimeJs.slice(runtimeJs.indexOf('const sub = '), runtimeJs.indexOf('\n', runtimeJs.indexOf('const sub = '))) +
      ';const R={buildShotSparkModel,shotRuleModel,cutLine,placePill,sub,PILL,bindScrub,presetGraph,observePresetHost,' +
      'axisLabel,fadeFill};return renderShotSpark;')(
      document, class{observe(){}}, ()=>({}), ()=>{});
  const observed = render(host, {wCg:[0,100,100,200], wAtMs:[0,100,600,600],
    wBreakBefore:[2], wTruncated:true, durationS:1});
  if (!observed || observed.flowCurve.some(v=>v!==null) ||
      observed.segs.length !== 2 || !host.innerHTML.includes('Incomplete curve'))
    throw new Error('Actual observation times and breaks cannot fabricate startup rates');
  // The reference shot: 0.4 g/s, a +3 g hand contact at 8.5 s whose leading
  // edge slipped through as weight before the firmware held the rest.
  const ramp = Array.from({length:201},(_,i)=>Math.round(Math.max(0,(i-50)*4))),
    rampAt = Array.from({length:201},(_,i)=>i*100),
    touched = ramp.map((v,i)=>v+(i===85||i===92?150:i>85&&i<92?300:0)),
    held = [86,87,88,89,90,91,92];
  const contact = render(host, {wCg:touched, wAtMs:rampAt, wHeld:held, durationS:20});
  const contactChart = host.innerHTML;
  if (!contact || contact.maxFlow > 0.6 || contact.segs.length !== 1 ||
      contact.held.length !== 7 || contact.pts.some((p)=>p.t>8.55&&p.t<9.25) ||
      held.some((i)=>contact.flowCurve[i]!==null) ||
      !contactChart.includes('class="shotHeld"') ||
      render(host, {wCg:touched, wAtMs:rampAt, durationS:20}).maxFlow < 2.5)
    throw new Error('Held readings must stay off the weight line and out of every flow window');
  const clean = render(host, {wCg:ramp, wAtMs:rampAt, wHeld:[], durationS:20});
  const spurt = render(host, {wCg:ramp.map((v,i)=>v+(i>=120?140:i===119?70:0)),
    wAtMs:rampAt, wHeld:[], durationS:20});
  if (Math.abs(clean.maxFlow-0.4) > 0.01 || host.innerHTML.includes('shotHeld') ||
      spurt.maxFlow < 1.7)
    throw new Error('Clean ramps and real spurts keep their measured flow');
  // The Live forecast pace curve: presetGraph must draw the quadratic from
  // the current point through the control point onto the dot at the streamed
  // SAP arrival (fc = [t*, cut line, ctrl t, ctrl g]).
  let paceMarkup = '';
  const paceHost = {id: 'paceTest', _w: 0, style: {}, dataset: {},
    get innerHTML() { return paceMarkup; },
    set innerHTML(v) { paceMarkup = v; },
    replaceChildren() { paceMarkup = ''; }, append() {}, setAttribute() {}};
  const paceGraph = new Function('document', 'const presetState={};' + helpers + preset +
      renderer +
      runtimeJs.slice(runtimeJs.indexOf('const sub = '), runtimeJs.indexOf('\n', runtimeJs.indexOf('const sub = '))) +
      ';return presetGraph;')(document);
  paceGraph(paceHost, {mode: 'active', prot: 3, t1: 27, t2: 36, wall: 60, dFast: 6,
      dSlow: 6, ceil: 42, goal: 36, floor: 33, fast: true, slow: true, firstDrop: 5,
      curveFast: 3, curveSlow: 6},  // rampCurveIndex('early'/'late', 'soft')
    {w: 300, h: 160, now: [25, 28], fc: [30, 36, 27.5, 32.5]});
  if (!/class="rvFc" d="M[^"]*Q[^"]*"/.test(paceMarkup) ||
      !paceMarkup.includes('class="rvFcDot"') || !paceMarkup.includes('class="rvNow"'))
    throw new Error('The live forecast must draw the pace curve onto its dot');
  const maximum = render(host, {wCg:Array(1201).fill(100),
    wAtMs:Array.from({length:1201},(_,i)=>i*50),durationS:60});
  if (maximum?.flowCurve.length !== 1201 || maximum.maxFlow !== 0)
    throw new Error('Full-capacity actual-time curves must remain usable');
  const basic = render(host, {wCg:[0,0,50,150,250], wAtMs:[0,1000,2000,3000,4000],
    durationS:4, firstDropS:2, dropCg:50});
  const combined = host.innerHTML;
  if (host.hidden || !basic || basic.timeMax !== 10 || basic.maxW !== 10 || basic.flowMax !== 1 ||
      combined.split('<div class="shotCurve">').length !== 2 ||
      !combined.includes('Weight (g)') || !combined.includes('Flow rate (g/s)') ||
      !combined.includes('class="shotSparkY"') || !combined.includes('class="shotSparkY shotSparkFlowY"') ||
      axes.length !== 2 || axes.some((axis) => axis.querySelectorAll().length !== 2) ||
      !combined.includes('<span class="shotYTick">1 g/s</span><span class="shotYTick">0 g/s</span>') ||
      !combined.includes('d="M1.5 34.5 L48.9 34.5 L60.8 1.5"') ||
      !combined.includes('d="M60.8 1.5 L84.4 1.5"') ||
      combined.indexOf('stroke="var(--gFlow)"') > combined.indexOf('stroke="var(--ok)"') ||
      combined.includes('shotDropOverlay') || markers.length !== 0 || combined.includes('shotFirstDrop') ||
      combined.indexOf('class="shotGrid"') > combined.indexOf('class="shotTrace"') ||
      !combined.includes('M1.5 1.5V34.5M238.5 1.5V34.5') ||
      fills() !== 3 || (combined.match(/<linearGradient /g) || []).length !== 3 || combined.includes('fill-opacity') ||
      plot.style['--shot-plot-min'] !== '1.10rem' ||
      keys[0].style.background !== 'var(--ok)' || keys[1].style.background !== 'var(--gFlow)' ||
      ticks.map(item=>item.textContent).join('|')!=='0 s|10 s') {
    throw new Error('The combined shot chart must keep both axes, draw flow under weight with fading fills, and plot measured rates at window midpoints');
  }
  if (stretched(combined).includes('<circle') || !combined.includes('d="M48.9 32.9h0"') ||
      !combined.includes('stroke-linecap="round"'))
    throw new Error('Event annotations must use line-width non-scaling strokes, not stretched filled circles');
  // The cursor starts at the stop point: both dots at 4 s, flow hidden past its last estimate.
  if (!combined.includes('<svg class="shotSpark shotSparkCursor" aria-hidden="true"><g class="shotCursor">') ||
      (combined.match(/class="rvHandle/g) || []).length !== 2 || !combined.includes('class="rvPill"') ||
      plot.attrs['aria-valuenow'] !== '4.0' || plot.attrs['aria-valuetext'] !== '4.0 s \u00B7 2.5 g \u00B7 \u2014 g/s' ||
      dots[1].attrs.cx !== dots[0].attrs.x1 || dots[2].attrs.visibility !== 'hidden')
    throw new Error('The weight/flow chart must carry the preset chart\u2019s cursor: a shared-time guide, a weight and a flow dot, and one pill');
  for (const interruption of [
    {wBreakBefore:[3]}, {wCg:[0,0,50,NaN,250,350,450]},
    {atmS:2.5,atmClearedS:3.5},
  ]) {
    const broken=render(host,{wCg:[0,0,50,150,250,350,450],
      wAtMs:[0,1000,2000,3000,4000,5000,6000],durationS:6,firstDropS:2,...interruption});
    if(broken.flowStart!==null || !host.innerHTML.includes('d="M1.5 34.5 L48.9 34.5"'))
      throw new Error('Startup reference must not bridge rejection, tare or scale-loss gaps');
  }
  const annotations=render(host,{wCg:[0,100,200],wAtMs:[0,500,1000],durationS:1,endS:1,endCg:500});
  if(stretched(host.innerHTML).includes('<circle') || host.innerHTML.includes('d="M25.2 18.0h0"') ||
      annotations.segs.length!==1 || annotations.pts.at(-1).cg!==200 ||
      annotations.maxFlow!==2 || annotations.flowCurve[2]!==2 ||
      new Function(helpers+';return shotDisplayActualG(5,[0,100,200]);')()!==5)
    throw new Error('Weight trace must end at its last sample while summary yield and flow stay unchanged');
  const single = render(host, {wCg:[0,100,200],wAtMs:[0,500,1000],durationS:1});
  if(single.flowSegs[0].pts.length!==1 || stretched(host.innerHTML).includes('<circle') ||
      !host.innerHTML.includes('d="M13.4 1.5h0"'))
    throw new Error('A single supported flow estimate must render as a point without invented endpoints');
  const dripShot={wCg:[0,100,200,240,250],wAtMs:[0,500,1000,1500,2000],
    durationS:1,endS:1,endCg:250};
  const drip=render(host,dripShot),dripChart=host.innerHTML;
  if(drip.flowCurve.join('|')!=='||2|1.4|0.5' || drip.maxFlow!==2 || drip.segs[0].pts.length!==3 ||
      drip.dripSegs[0].pts.length!==3 || drip.finalPoint.t!==2 || drip.finalPoint.cg!==250 ||
      (dripChart.match(/class="shotTrace shotDripTrace"/g)||[]).length!==2 ||
      !dripChart.includes('stroke-dasharray="3 3"/>') || dripChart.includes('opacity=".55"') ||
      markers.length!==1 || markers[0].className!=='shotFinalPoint' ||
      fills()!==2 || drip.dripFlowSegs[0].pts[0].t!==1 ||
      (dripChart.match(/shotDropOverlay/g)||[]).length!==1 || markers[0].style.width!=='6px' || markers[0].style.height!=='6px')
    throw new Error('Real drip readings must form unfilled dashed weight/flow tails and a fixed-size weight ring');
  const interrupted=render(host,{...dripShot,wBreakBefore:[3]});
  if(interrupted.dripSegs[0].pts[0].t!==1.5 || interrupted.dripFlowSegs.length ||
      interrupted.flowCurve.slice(3).some(v=>v!==null))
    throw new Error('Drip tail must not connect across a recorded interruption');
  for(const extra of [{wTruncated:true},{endCg:300}]) {
    if(render(host,{...dripShot,...extra}).finalPoint || markers.length)
      throw new Error('An incomplete or unsettled tail cannot claim a final-yield marker');
  }
  const longDrip=render(host,{...dripShot,wAtMs:[0,500,1000,10000,11000]});
  if(longDrip.timeMax!==20 || longDrip.maxFlow!==2)
    throw new Error('Both chart time axes must include drip times without changing extraction Max flow');
  const spikeDrip=render(host,{...dripShot,wCg:[0,100,200,1000,2000]});
  if(spikeDrip.maxFlow!==2 || spikeDrip.flowMax!==18 || spikeDrip.flowCurve[4]!==18)
    throw new Error('Drip rates must fit the axis and CSV without becoming extraction Max flow');
  const crossCut=render(host,{wCg:[0,100,200,220,260],wAtMs:[0,500,1000,1100,1600],endS:1,durationS:1});
  if(crossCut.dripFlowSegs[0].pts[0].t!==1 || crossCut.dripFlowSegs.some(s=>s.pts.some(p=>p.t<1)))
    throw new Error('Midpoint estimates from crossing windows must never draw dashed flow before cutoff');
  const recovered=render(host,{wCg:[0,100,200,300,400,500],wAtMs:[0,500,1000,1500,2000,2500],endS:1,durationS:1,wBreakBefore:[3]});
  if(recovered.dripFlowSegs[0].pts[0].t!==2 || recovered.flowCurve[3]!==null || recovered.flowCurve[4]!==null)
    throw new Error('Drip flow must regain a full supported window after an interruption without bridging cutoff');
  render(host, {wCg:[0,0,0],wAtMs:[0,1000,2000],durationS:2});
  if(host.hidden || markers.length || fills() ||
      !host.innerHTML.includes('d="M13.4 34.5 L37.0 34.5"'))
    throw new Error('Flat measured zero flow must remain visible');
  const tared=render(host,{wCg:[0,100,200],wAtMs:[0,500,1000],durationS:10,
    firstDropS:2,dropCg:50,tareS:3.4});
  if(tared.tareS!==3.4 || markers.length || host.innerHTML.includes('shotTareIcon'))
    throw new Error('The weight-only chart must not draw first-drop or tare markers; the card keeps them as fields');
  render(host,{wCg:[0,100,200],wAtMs:[0,500,1000],durationS:3,tareS:1});
  if(markers.length)
    throw new Error('Whole-second tare times must not draw a weight-only marker');
  const rounded=render(host,{wCg:[0,1200,2500,3900],wAtMs:[0,3775,7550,11325],
    durationS:15.1,goalG:36});
  if(rounded.timeMax!==20 || rounded.maxW!==40 || rounded.flowMax!==4 ||
      rounded.maxFlow<=3.7 || rounded.maxFlow>=3.8 ||
      plot.style['--shot-plot-min']!=='4.40rem' ||
      !host.innerHTML.includes('<span class="shotYTick">4 g/s</span><span class="shotYTick">3 g/s</span>'))
    throw new Error('Non-multiple domains must round up; flow takes the step that fits the shared weight grid');
  const exact=render(host,{wCg:[1000,2000,3000,4000],wAtMs:[0,10000,20000,30000],durationS:40});
  if(exact.timeMax!==40 || exact.maxW!==40 || exact.flowMax!==1 ||
      plot.style['--shot-plot-min']!=='4.40rem' ||
      !host.innerHTML.includes('<span class="shotYTick">1 g/s</span><span class="shotYTick">0.75 g/s</span>'))
    throw new Error('Exact axis boundaries must retain compact minimums and a quarter-step flow axis');
  render(host, null);
  if (!host.hidden || host.innerHTML) throw new Error('Missing shot data must still hide the charts');
}

{
  const clearSource = runtimeJs.slice(runtimeJs.indexOf('function clearShotHero('),
      runtimeJs.indexOf('function checkFirmwareReload('));
  const hero = {hidden: false};
  new Function('$','runShot', clearSource + ';clearShotHero();')(id => hero, () => {});
  if (!hero.hidden) {
    throw new Error('Clearing Home must hide the shot hero');
  }
  const heroElements=new Map(),lookup=id=>{
    if(!heroElements.has(id))heroElements.set(id,{hidden:false,textContent:'',dataset:{},style:{tp:null,setProperty(k,v){this[k==='--tp'?'tp':k]=v;}},
      classList:{toggle(){},contains(){return false}},setAttribute(){},removeAttribute(){},prepend(){},
      replaceChildren(...n){this.kids=n;this.textContent=n.map(x=>typeof x==='string'?x:x.textContent).join('');},
      append(...n){this.kids.push(...n);this.textContent+=n.map(x=>typeof x==='string'?x:x.textContent).join('');},
      querySelector(){return svgStub}, querySelectorAll(){return []}});
    return heroElements.get(id);
  };
  const svgStub={insertAdjacentHTML(_,markup){this.appended=(this.appended||'')+markup;},
    children:[{setAttribute(){}},{setAttribute(){}},{}]};
  let tintMode=null,heroOverride=null;
  const rule={mode:'active',t1:27,t2:36,prot:3,wall:60,d:5,ceil:38.5};
  const env={liveRuleModel:null,model:null,reduce:false,heroMode:'colorful'},ruleArgs=[];
  const paint=(d)=>new Function('$','buildShotSparkModel','formatShotEnded','ms','localStorage','document',
    'homeStatusCache','liveRuleModel','uiState','shotRuleModel','scheduleShotPaint','sub','window','shotFsActive',
    'bindScrub','placePill','PILL','observePresetHost','d',
    blockAt(runtimeJs,'function fadeFill(')+blockAt(runtimeJs,'const RAMP_CURVES =')+
    blockAt(runtimeJs,'function rampDg(')+
    blockAt(runtimeJs,'function cutLine(')+'const PACE_SOFT = 0.18;\n'+
    blockAt(runtimeJs,'function referencePace(')+
    blockAt(runtimeJs,'function escHtml(')+blockAt(runtimeJs,'function paceStartNote(')+
    blockAt(runtimeJs,'function flowLayer(')+blockAt(runtimeJs,'function presetGraph(')+
    blockAt(runtimeJs,'function verdictLabel(')+
    runtimeJs.slice(runtimeJs.indexOf('function renderShotHero('),runtimeJs.indexOf('function checkFirmwareReload('))+
    ';return renderShotHero({clockMs:d.elapsedMs,...d});')(lookup,()=>env.model,()=>'',
    (v,n)=>(v/1000).toFixed(n),{getItem:(k)=>k==='obbwHeroTint'?tintMode:heroOverride,setItem(){}},
    {createElement:()=>({style:{},textContent:''})},
    ()=>env.heroMode?{config:{heroCardMode:env.heroMode}}:null,
    env.liveRuleModel,{shotHistory:{shots:[]}},r=>(ruleArgs.push(r),env.shotRule||null),()=>null,
    (t,vals)=>t.replace(/\{(\d)\}/g,(_,i)=>vals[+i]),{matchMedia:()=>({matches:env.reduce})},false,
    ()=>{},() => {}, '', ()=>{}, d);
  const card={live:true,weight:2,goal:36,elapsedMs:1000,firstDropMs:0,tareMs:null,averageFlowGps:null,shotType:'auto'};
  const heroEl=()=>lookup('shotHero');
  paint({...card,weight:36.2,elapsedMs:36800});
  if(lookup('shotHeroWeight').textContent!=='36.2 g'||
      lookup('shotHeroElapsed').textContent!=='36.8 s'||!lookup('shotHeroPills').hidden)
    throw new Error('Home must label units and keep pills off a live shot');
  if(lookup('shotHeroState').textContent!=='Brewing'||heroEl().dataset.tint!=='neutral')
    throw new Error('A live shot without a cut line must read Brewing on the neutral colour');
  // An active cut line names its target and both holds on the hero chart.
  env.model={segs:[{pts:[{t:0,cg:0},{t:30,cg:30000}]}],dripSegs:[],flowSegs:[],
    dripFlowSegs:[],flowCurve:[1],timeMax:30,maxW:30};
  env.liveRuleModel={...rule,goal:36,floor:33,ceil:42,dSlow:6,firstDrop:5,curveFast:3,curveSlow:6,dFast:6};
  paint({...card,weight:20,wCg:[5000],wAtMs:[0]});
  const chartLabels=(lookup('shotHeroSpark').kids||[]).map(x=>x.textContent);
  if(!chartLabels.includes('Target 36 g')||!chartLabels.includes('Up to 42 g')||
      !chartLabels.includes('Down to 33 g'))
    throw new Error('The hero chart must name the target and both cut holds');
  if(!(lookup('shotHeroSpark').kids||[]).some(x=>x.className==='hold mid'&&
      x.textContent==='Target 36 g'&&x.style.left==='68.48%'))
    throw new Error('The hero chart target must sit centered over the on-recipe window');
  env.model=null;env.liveRuleModel=null;
  // A finished shot reads its verdict in the label, takes its colour, and keeps a cut pill.
  const done={...card,live:false,weight:38.1,verdict:'fast',cutG:39.4};
  paint(done);
  if(lookup('shotHeroState').textContent!=='Fast'||heroEl().dataset.tint!=='fast'||
      lookup('shotHeroCut').textContent!=='Extended')throw new Error('A fast shot must read Fast, turn red and keep Extended');
  // The stored shot's own rule draws the cut line on first paint, without the Stats records.
  paint({...done,rule:{fastBeforeS:27,slowAfterS:36,fastRampS:2,slowRampS:2}});
  if(ruleArgs.length!==1||ruleArgs[0].goalG!==36||ruleArgs[0].fastBeforeS!==27||
      ruleArgs[0].shotType!=='auto'||ruleArgs[0].firstDropS!==0)
    throw new Error('A finished shot must build its cut line from the card rule');
  paint({...done,verdict:'slow',cutG:34});
  if(lookup('shotHeroState').textContent!=='Slow'||heroEl().dataset.tint!=='slow'||lookup('shotHeroCut').textContent!=='Stopped early')
    throw new Error('A slow shot must read Slow, turn blue and keep Stopped early');
  paint({...done,verdict:'on_recipe',cutG:36});
  if(lookup('shotHeroState').textContent!=='On recipe'||heroEl().dataset.tint!=='on_recipe'||!lookup('shotHeroCut').hidden)
    throw new Error('An on-recipe shot cut at the target needs no cut pill');
  paint({...done,channeling:true});
  if(lookup('shotHeroCut').textContent!=='Channeling')throw new Error('Channeling must replace the cut pill');
  paint({...done,verdict:null,cutG:null,endReason:'wall_limit'});
  if(lookup('shotHeroState').textContent!=='Max time'||heroEl().dataset.tint!=='neutral')
    throw new Error('A wall stop must read Max time');
  tintMode='brand';
  paint(done);
  if(heroEl().dataset.tint!=='brand'||lookup('shotHeroState').textContent!=='Fast')
    throw new Error('Brand colours must keep the brand gradient and still name the verdict');
  tintMode=null;
  // Live colour in forecast mode is the firmware's: class + blend percent.
  env.liveRuleModel=rule;
  paint({...card,elapsedMs:15000,tintClass:'on_recipe',tintPct:40});
  if(heroEl().dataset.tint!=='on_recipe'||heroEl().style.tp!=='40%')
    throw new Error('A live forecast tint must paint the streamed class at its blend percent');
  paint({...card,elapsedMs:15000,tintClass:'neutral',tintPct:0});
  if(heroEl().dataset.tint!=='neutral'||lookup('shotHeroState').textContent!=='Brewing')
    throw new Error('A young forecast stays neutral coffee');
  paint({...card,elapsedMs:15000});
  if(heroEl().dataset.tint!=='neutral')
    throw new Error('A stream without tint fields must stay neutral (legacy)');
  paint({...card,elapsedMs:24000,tintClass:'on_recipe',tintPct:60,fcEndMs:29400});
  if(!/^~29 s$/.test(lookup('shotHeroState').textContent))
    throw new Error('A confident green shows the landing estimate');
  paint({...card,elapsedMs:26000,tintClass:'slow',tintPct:80,weight:28,averageFlowGps:1.4,expectedEndMs:39500});
  if(!/Slow — about 42 g at 36 s/.test(lookup('shotHeroState').textContent))
    throw new Error('A flowing slow shot shows the projected weight at the window end');
  paint({...card,elapsedMs:26000,tintClass:'slow',tintPct:80,weight:28,averageFlowGps:0.5,tintNote:'stalled'});
  if(!/Stalled — tracking to ~45 g/.test(lookup('shotHeroState').textContent))
    throw new Error('A stalled shot tracks to its projected wall weight');
  paint({...card,elapsedMs:26000,tintClass:'slow',tintPct:80,weight:28,averageFlowGps:1.4,expectedEndMs:39500,tintNote:'recovered'});
  if(!/Slow — about 42 g at 36 s · Flow rose/.test(lookup('shotHeroState').textContent))
    throw new Error('A recovered slow shows the flow-rose note');
  paint({...card,elapsedMs:26000,tintClass:'fast',tintPct:100});
  if(heroEl().dataset.tint!=='fast'||lookup('shotHeroState').textContent!=='Running fast')
    throw new Error('A streamed fast tint reads Running fast');
  paint({...card,elapsedMs:26000,tintClass:'on_recipe',tintPct:40,fcEndMs:29400});
  // window mode ignores the stream's tint and uses the clock
  tintMode='window';
  paint({...card,elapsedMs:1500});
  paint({...card,elapsedMs:30000,tintClass:'fast',tintPct:100});
  if(heroEl().dataset.tint!=='on_recipe')throw new Error('By time window must keep its clock colour');
  if(heroEl().style.tp!=='100%')throw new Error('Non-forecast modes keep the full blend');
  tintMode=null;
  tintMode='window';
  paint({...card,elapsedMs:2000});
  paint({...card,elapsedMs:30000});
  if(heroEl().dataset.tint!=='on_recipe'||lookup('shotHeroState').textContent!=='In window')
    throw new Error('By time window must turn green inside the window');
  paint({...card,elapsedMs:31000,shotType:'manual'});
  if(heroEl().dataset.tint!=='neutral')throw new Error('Only automatic shots follow the cut line colours');
  tintMode=null;
  env.liveRuleModel=null;
  paint({...done,verdict:null,cutG:null,scaleAvailable:false,weight:null,
    scaleLoss:{state:'finished',stopAtS:31,estimatedG:35.6}});
  if(lookup('shotHeroWeight').textContent!=='~36 g'||lookup('shotHeroCut').textContent!=='No scale')
    throw new Error('A scale-loss stop must show its estimate and the No scale pill');
  paint({...card,scaleAvailable:false,weight:null,elapsedMs:27400,firstDropMs:2000,averageFlowGps:1.5});
  if(lookup('shotHeroWeight').textContent!=='27.4 s'||
      !lookup('shotHeroElapsed').hidden||
      lookup('shotHeroMode').hidden||lookup('shotHeroMode').textContent!=='No scale')
    throw new Error('A shot started without a scale must turn the hero into a timer');
  if(!lookup('shotHeroFlow').hidden)throw new Error('A shot without a scale must not show a flow');
  // Flow: the mean of the chart's last second while brewing (older samples
  // drop out); finished shows the average.
  const flowText=d=>(paint(d),lookup('shotHeroFlow').hidden?null:lookup('shotHeroFlow').textContent);
  const brewing={...card,weight:20,elapsedMs:20000,firstDropMs:6000,wAtMs:[0,500,1000,1500,2000]};
  env.model={segs:[],flowCurve:[0.4,1.2,null,1.48,null]};
  if(flowText(brewing)!=='1.3 g/s'||flowText({...brewing,wAtMs:[0,500,1000,1500,2600]})!==null||
      flowText({...brewing,touchHold:true})!==null||flowText({...brewing,firstDropMs:null})!==null||
      flowText({...brewing,weight:null})!==null||
      flowText({...done,firstDropMs:6000,averageFlowGps:1.43})!=='1.4 g/s'||
      flowText({...done,firstDropMs:6000,averageFlowGps:null})!==null)
    throw new Error('Home must show the computed flow over its last second, and the average once finished');
  env.model={segs:[],flowCurve:[null,null,null,-0.4,null]};
  if(flowText(brewing)!=='0.0 g/s')throw new Error('Home must never show a negative flow');
  env.model={segs:[],flowCurve:[null,null,null,1.63,null]};
  flowText(brewing);
  if(lookup('shotHeroFlow').kids.map(k=>typeof k==='string'?k:k.textContent).join('|')!=='1.6| g/s')
    throw new Error('Home must set the flow unit smaller, like g and s');
  // Finishing fades the live flow out and the average in, 100 ms each; values
  // arriving mid-fade replace the pending one.
  const fx=lookup('shotHeroFlow'),anims=[];
  fx.animate=(k,o)=>{const a={k,o,cancel(){a.cancelled=true},finished:{then:ok=>(a.ok=ok)}};anims.push(a);return a;};
  env.model={segs:[],flowCurve:[null,1.2,null,1.48,null]};
  flowText(brewing);
  flowText({...done,firstDropMs:6000,averageFlowGps:1.43});
  flowText({...done,firstDropMs:6000,averageFlowGps:1.61});
  if(anims.length!==1||anims[0].o.duration!==100||anims[0].k[1].opacity!==0||fx.textContent!=='1.3 g/s')
    throw new Error('Home must hold the live flow while it fades out');
  anims[0].ok();
  flowText({...done,firstDropMs:6000,averageFlowGps:1.61});
  if(fx.textContent!=='1.6 g/s'||anims.length!==2||anims[1].o.duration!==100||anims[1].k[1].opacity!==1||
      !anims[0].cancelled)
    throw new Error('Home must fade the newest average in once the live flow is gone');
  env.reduce=true;
  flowText({...done,firstDropMs:6000,averageFlowGps:1.82});
  if(fx.textContent!=='1.8 g/s'||anims.length!==2)throw new Error('Reduced motion must swap the flow at once');
  env.reduce=false;
  delete fx.animate;
  // The chart draws the computed flow as a ±1 s mean, an unoutlined area low
  // under the weight; the drip tail is fainter.
  const chart={innerHTML:'',setAttribute(){},replaceChildren(){},append(){},prepend(){}},
    pts=a=>a.map(([t,cg])=>({t,cg}));
  const smoothFlow=new Function(blockAt(runtimeJs,'function smoothFlow(')+';return smoothFlow;')();
  // The sliding ±1 s window matches the direct mean over every in-range point.
  for(let run=0;run<20;run++){
    const p=[];for(let t=0,i=0;i<60;i++)p.push({t:t+=Math.random()*(run%3?0.4:1.6),cg:Math.random()*900});
    const naive=p.map(q=>{const n=p.filter(r=>Math.abs(r.t-q.t)<=1);return [q.t,n.reduce((a,r)=>a+r.cg,0)/n.length/100];});
    smoothFlow({pts:p}).forEach(([t,v],i)=>{if(t!==naive[i][0]||Math.abs(v-naive[i][1])>1e-9)throw new Error('Flow smoothing must equal the ±1 s mean');});
  }
  // RV-11: points exactly 1 s away are inside the window on both sides.
  const edge=smoothFlow({pts:[{t:0,cg:100},{t:1,cg:300},{t:2,cg:800}]});
  if(edge.length!==3||[[0,2],[1,4],[2,5.5]].some(([t,v],i)=>edge[i][0]!==t||Math.abs(edge[i][1]-v)>1e-9))
    throw new Error('Flow smoothing must include the ±1 s window boundary');
  new Function('fadeFill','document','h','m','smoothFlow',blockAt(runtimeJs,'function renderHeroChart(')+
    ';renderHeroChart(h,m,null,36,"");')(()=>'',{createElement:()=>({style:{}})},chart,
    {segs:[{pts:pts([[6,0],[27,3500]])}],dripSegs:[{pts:pts([[27,3500],[29,3600]])}],timeMax:30,maxW:40,
      flowSegs:[{pts:pts([[10,100],[10.5,200],[11,150]])}],
      dripFlowSegs:[{pts:pts([[27,150],[29,0]])}]},smoothFlow);
  if(!chart.innerHTML.includes('<path d="M33.33 80.80L35.00 80.80L36.67 80.80V100H33.33Z" fill="url(#heroFlow)"/>')||
      !chart.innerHTML.includes('<path d="M90.00 80.80L96.67 100.00V100H90.00Z" fill="var(--on)" fill-opacity=".06"/>')||
      /stroke-opacity="\.[56]"/.test(chart.innerHTML)||
      chart.innerHTML.indexOf('url(#heroFlow)"/>')>chart.innerHTML.indexOf('stroke-width="2.4"'))
    throw new Error('The Home chart must draw a smoothed flow area under the weight, with a fainter drip tail');
  env.model=null;
  if(flowText(brewing)!==null)throw new Error('Without a flow sample Home must hide the flow');
  paint({...card,scaleAvailable:true});
  if(lookup('shotHeroWeight').textContent!=='2.0 g'||
      lookup('shotHeroElapsed').hidden||!lookup('shotHeroMode').hidden)
    throw new Error('A scale shot must keep the weight layout and no BBW pill');
  paint({...card,touchHold:true});
  if(lookup('shotHeroTouch').hidden||lookup('shotHeroTouch').textContent!=='touch hold')
    throw new Error('A live sustained touch must surface on the shot hero');
  paint({...card,touchHold:true,live:false});
  if(!lookup('shotHeroTouch').hidden)throw new Error('The touch-hold pill must be live-only');
  paint({...card,touchHold:true,scaleAvailable:false,weight:null,elapsedMs:27400,firstDropMs:2000,averageFlowGps:1.5});
  if(!lookup('shotHeroTouch').hidden)throw new Error('A shot without a scale must never claim a touch hold');
  paint({...card});
  if(!lookup('shotHeroTouch').hidden)throw new Error('The touch-hold pill needs the firmware flag');
  // The Technical hero is the default: the preset graph under the numbers,
  // the SAP zone glow and forecast cut from the same card fields, the calm
  // Brewing label on a good forecast, and the verdict zone plus cursor once
  // ended. Machine choice and device override pick between the two cards.
  const spark=()=>lookup('shotHeroSpark');
  spark().clientWidth=390;
  env.model={segs:[{pts:[{t:0,cg:0},{t:15,cg:2000}]}],dripSegs:[],flowSegs:[],dripFlowSegs:[],
    held:[],pts:[{t:0,cg:0},{t:15,cg:2000}],flowCurve:[1.4],timeMax:15,maxW:20,rawMaxW:20};
  env.liveRuleModel={...rule,goal:36,floor:33,ceil:42,dSlow:6,firstDrop:5,curveFast:3,curveSlow:6,dFast:6};
  env.heroMode=null;
  paint({...card,elapsedMs:15000,tintClass:'on_recipe',tintPct:40,weight:20,wCg:[2000],wAtMs:[0],fcEndMs:29400});
  if(heroEl().dataset.hero!=='technical'||spark().dataset.zone!=='live'||
      !spark().innerHTML.includes('class="rvCut')||!spark().innerHTML.includes('Target 36 g')||
      !spark().innerHTML.includes('rvFcDot'))
    throw new Error('Without a machine choice the hero must be Technical with its graph and forecast');
  if(lookup('shotHeroState').textContent!=='Brewing')
    throw new Error('A technical on-recipe forecast keeps the calm Brewing label');
  env.liveRuleModel=null;
  env.shotRule={...rule,goal:36,floor:33,ceil:42,dSlow:6,firstDrop:5,curveFast:3,curveSlow:6,dFast:6};
  paint({...done,rule:{fastBeforeS:27,slowAfterS:36,fastRampS:2,slowRampS:2}});
  if(spark().dataset.zone!=='fast'||!(svgStub.appended||'').includes('class="shotCursor"'))
    throw new Error('An ended technical shot lights its verdict zone and keeps the cursor');
  env.heroMode='colorful';
  paint({...done,rule:{fastBeforeS:27,slowAfterS:36,fastRampS:2,slowRampS:2}});
  if(heroEl().dataset.hero!=='colorful'||spark().innerHTML.includes('rvCut'))
    throw new Error('The colorful machine choice must render the classic card');
  env.heroMode=null;heroOverride='colorful';
  paint({...done,rule:{fastBeforeS:27,slowAfterS:36,fastRampS:2,slowRampS:2}});
  if(heroEl().dataset.hero!=='colorful')throw new Error('A device override must win over the machine default');
  env.heroMode='colorful';heroOverride='technical';
  paint({...done,rule:{fastBeforeS:27,slowAfterS:36,fastRampS:2,slowRampS:2}});
  if(heroEl().dataset.hero!=='technical')throw new Error('An explicit device override must win both ways');
  heroOverride=null;env.shotRule=null;env.model=null;
  const chips=['Cut','Touch','Mode'].map(id=>html.indexOf('id="shotHero'+id+'"'));
  if(chips.some((pos,i)=>pos<0||(i&&pos<=chips[i-1])))throw new Error('Home shot pills must keep their reading order');
  const apply = new Function(runtimeJs.slice(runtimeJs.indexOf('function uiStreamFrame('),
    runtimeJs.indexOf('function startUiStream('))+';return uiStreamFrame;')();
  // The firmware's delta stream: one byte per step, or 0xFF/0x80 plus the LE absolute.
  const deltas=(at,cg,[t,w]=[0,0])=>{const b=[];at.forEach((a,i)=>{const dt=a-t,dw=cg[i]-w;
    if(dt>=0&&dt<255)b.push(dt);else b.push(255,a&255,a>>8);
    if(dw>-128&&dw<128)b.push(dw&255);else b.push(128,cg[i]&255,(cg[i]>>8)&255);t=a;w=cg[i];});
    return Buffer.from(b).toString('base64');};
  const snapshot={v:1,boot:7,seq:1,snapshot:true,cycle:19,shotId:0,stale:false,
    phase:'active',cursor:2,card:{valid:true,live:true,weight:1,elapsedMs:1000,averageFlowGps:null,firstDropMs:null,tareMs:null},
    curve:{wD:deltas([100,700],[100,100]),wBreakBefore:[],wHeld:[],dropS:null,endS:null}};
  const first=apply(null,snapshot);
  // An increment names only its sequence and cursor; its fragment names the
  // point it continues.
  const update={seq:2,cursor:4,card:{elapsedMs:1100},
    curve:{wRef:[700,100],wD:deltas([800,900],[100,120],[700,100]),wBreakBefore:[2],wHeld:[3],dropS:.8}};
  const next=apply(first,update);
  if(next.curve.wCg.join()!=='100,100,100,120'||next.curve.wAtMs.join()!=='100,700,800,900'||
      next.curve.wBreakBefore.join()!=='2'||next.curve.wHeld.join()!=='3'||next.curve.dropS!==.8||
      next.curve.endS!==null||next.cycle!==19||next.phase!=='active'||next.curve.wD!==undefined||first.cursor!==2)
    throw new Error('Stream must atomically append all samples, equal weights, breaks, held readings and metadata');
  if(apply(next,update)!==next)throw new Error('Duplicates must not append');
  const empty={wD:'',wBreakBefore:[],wHeld:[]};
  const delta=apply(next,{seq:3,cursor:4,card:{weight:2.5},curve:{...empty,endS:1.2}});
  if(delta.card.weight!==2.5||delta.card.elapsedMs!==1100||delta.card.valid!==true||
      delta.curve.wCg.length!==4||delta.curve.endS!==1.2||delta.curve.dropS!==.8||next.card.weight!==1)
    throw new Error('Card and curve-scalar deltas must merge over the previous frame');
  for(const bad of [{weight:'2'},{elapsedMs:-1},{valid:null}]){
    let rejected=false;
    try{apply(next,{seq:3,cursor:4,card:bad,curve:empty})}
    catch(_){rejected=true}
    if(!rejected)throw new Error('A merged card must pass the snapshot card checks');
  }
  const quiet=apply(delta,{seq:4,cursor:4,phase:'pending',card:{},curve:empty});
  if(quiet.phase!=='pending'||quiet.stale!==false||quiet.card.weight!==2.5)
    throw new Error('An empty card delta must keep the card, and the envelope its unsent fields');
  const corrected=apply(next,{...snapshot,seq:3,curve:{...snapshot.curve,dropS:null}});
  if(corrected.cursor!==2||corrected.curve.dropS!==null)throw new Error('A marker-correcting snapshot must replace the curve');
  const reboot=apply(next,{...snapshot,boot:8});
  if(reboot.boot!==8||reboot.cursor!==2)throw new Error('Boot snapshots must discard old cursors');
  for(const [prev,bad] of [[first,{...update,seq:3}],[null,update],
    [first,{...update,curve:{...update.curve,wRef:[700,99]}}],
    [first,{...update,curve:{...update.curve,wRef:undefined}}],
    [first,{...update,curve:{...update.curve,wRef:['700',100]}}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wRef:[0,0]}}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wD:'!!'}}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wD:Buffer.from([255,1]).toString('base64')}}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wD:undefined}}],
    [first,{...snapshot,seq:3,cursor:1202}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wD:deltas([700,100],[100,100])}}],
    [first,{...snapshot,seq:3,curve:{...snapshot.curve,wD:deltas([100,700],[32767,32800])}}],
    [first,{...snapshot,seq:3,card:{...snapshot.card,weight:NaN}}],
    [first,{...snapshot,seq:3,card:{...snapshot.card,scaleAvailable:'false'}}],
    [first,{...snapshot,seq:3,card:{...snapshot.card,scaleAvailable:null}}],
    ...[undefined,-1,NaN,Infinity,60001,'0'].map(firstDropMs=>[first,{...snapshot,seq:3,card:{...snapshot.card,firstDropMs}}]),
    ...[-1,NaN,'0'].map(tareMs=>[first,{...snapshot,seq:3,card:{...snapshot.card,tareMs}}])]){
    let rejected=false;try{apply(prev,bad)}catch(_){rejected=true}
    if(!rejected)throw new Error('Invalid/gapped stream must request recovery');
  }
  const maximum=apply(null,{...snapshot,cursor:1201,curve:{wD:deltas(Array.from({length:1201},(_,i)=>i*50),
    Array(1201).fill(100)),wBreakBefore:[],wHeld:[],wTruncated:true}});
  if(maximum.curve.wCg.length!==1201||maximum.curve.wAtMs[1200]!==60000)
    throw new Error('Maximum curve must stay bounded and complete');
  const events=apply(null,{...snapshot,card:{...snapshot.card,firstDropMs:0,tareMs:60000}});
  if(events.card.firstDropMs!==0||events.card.tareMs!==60000||first.card.firstDropMs!==null)
    throw new Error('Scalar events must distinguish measured zero from unavailable data');
  const timed=apply(null,{...snapshot,card:{...snapshot.card,scaleAvailable:false}});
  if(timed.card.scaleAvailable!==false)
    throw new Error('The no-scale card flag must survive stream framing');
  const streamSource=fs.readFileSync(path.join(sketchDir,'network/OpenBrewByWeightUiStream.inc'),'utf8');
  const handler=streamSource.slice(streamSource.indexOf('esp_err_t OpenBrewByWeightNetwork::uiStreamHandler('),
    streamSource.indexOf('void OpenBrewByWeightNetwork::serviceUiStream('));
  if(handler.includes('sendUiStream(')||!handler.includes('session->resync = true;')||
      !streamSource.includes('uiStreamWorkPending_.exchange(true')||
      !streamSource.includes('if (!uiStreamDispatchDue(uiStreamUrgent_.load(')||
      !streamSource.includes('uiStreamDispatchSamples_.store(samples'))
    throw new Error('Bind/resync must share the coalesced publication cadence');
  if(!streamSource.includes('setsockopt(session.fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));'))
    throw new Error('The UI stream socket must send frames without Nagle delay');
  if(!/rule\[392\] = "null";\n  if \(canonical\) \{\n    snprintf\(rule,/.test(streamSource)||
      !streamSource.includes('delta.field("rule", "%s", rule);')||
      !streamSource.includes('reinterpret_cast<const uint8_t *>(phase), strlen(phase)')||
      !streamSource.includes('delta.field("tintClass", "\\"%s\\"", control.activeCycle')||
      !streamSource.includes('delta.field("tintPct", "%u", control.activeCycle ?'))
    throw new Error('Only a stored shot card may carry its preset rule and live tint');
  const sockets=[],timers=new Map();let timerId=0,owner=true;
  class Socket{static OPEN=1;constructor(url){this.url=url;this.readyState=1;this.sent=[];sockets.push(this)}send(body){const m=JSON.parse(body);this.sent.push(m,...(m.ops||[]))}close(){this.readyState=3;this.onclose?.()}}
  const listeners={};
  const controller=new Function('WebSocket','location','document','window','setTimeout','clearTimeout',
    'requestAnimationFrame','webUiPollingActive','webUiClientId','activeView','$','webUiPowerSeconds','invalidateHomeStream',
    runtimeStateDecl+'let homeFrame=null,homeStale=true,homeReady,homeResolve=()=>{};function noteReachFail(){}'+runtimeJs.slice(runtimeJs.search(/let\s+shotWs\s*=/),runtimeJs.indexOf('function setHomeSub('))+
    ';return{start:startUiStream,stop:stopUiStream,frame:()=>shotFrame,stale:()=>shotStale};')(
    Socket,{protocol:'http:',host:'device.local'},
    {hidden:false,addEventListener:(name,fn)=>listeners[name]=fn},
    {addEventListener:(name,fn)=>listeners[name]=fn},
    (fn,delay)=>{timers.set(++timerId,{fn,delay});return timerId},id=>timers.delete(id),()=>1,
    ()=>owner,'0123456789abcdef','stats',()=>null,()=>0,()=>{});
  controller.start();controller.start();
  if(sockets.length!==1)throw new Error('Navigation must preserve one socket');
  const socket=sockets[0];socket.onopen();
  if(socket.sent[0].op!=='bind'||socket.sent[0].client!=='0123456789abcdef')throw new Error('Bind requires claimed identity');
  socket.onmessage({data:JSON.stringify(snapshot)});
  if(controller.stale()||controller.frame().seq!==1)throw new Error('Initial snapshot must replace stale cache');
  socket.onmessage({data:JSON.stringify({...update,seq:4})});
  if(!controller.stale()||socket.sent.at(-1).op!=='resync')throw new Error('Gap must mark stale and request one resync');
  socket.onmessage({data:JSON.stringify({...snapshot,seq:5})});
  if(controller.stale())throw new Error('Replacement snapshot must recover');
  socket.close();
  const retry=[...timers.values()].find(t=>t.delay>=400&&t.delay<=600);
  if(!retry||!controller.stale())throw new Error('Failure must freeze state and back off');
  retry.fn();const rebound=sockets.at(-1);rebound.onopen();
  if(rebound.sent[0].cycle!==19||rebound.sent[0].boot!==7)throw new Error('Reconnect must preserve observed identity');
  rebound.onmessage({data:JSON.stringify({...snapshot,phase:'pending',card:{...snapshot.card,live:false}})});
  owner=false;controller.stop();controller.start();
  if(sockets.length!==2||rebound.readyState!==3)throw new Error('Inactive owner cannot retain/reopen stream');
  if(codeIncludes(runtimeJs, 'let shotTick=')||codeIncludes(runtimeJs, 'runShot(live?s:0)')||
      !codeIncludes(runtimeJs, 'stopUiStream();')||!codeIncludes(js, 'startUiStream();')||
      !codeIncludes(runtimeJs, "op:'bind',client:webUiClientId")||
      !codeIncludes(runtimeJs, '"op":"resync"'))
    throw new Error('Firmware timer and stream must follow ownership/visibility lifecycle');
}

{
  const assert=require('assert').strict,frames=new Map(),labels=[];
  let now=0,id=0,owner=true,reducedMotion=false;
  const weight={},animations=[];
  const document={hidden:false};
  const timer=new Function('performance','requestAnimationFrame','cancelAnimationFrame','document',
    'webUiPollingActive','heroNum','$','ms','paintUiStream','window',
    "let shotStale=false,activeView='home',shotFrame=null;"+
    runtimeJs.slice(runtimeJs.search(/let\s+shotClock\s*=/),runtimeJs.indexOf('// The diagnostic stream rides'))+
    ';return{sync:f=>{syncShotClock(f);shotFrame=f;revealShotClockFinish()},elapsed:shotClockElapsed,schedule:scheduleShotClock,'+
    'finish:finishShotClock,ending:()=>shotClockFinish,clock:()=>shotClock,stale:()=>shotStale,setStale:v=>shotStale=v,view:v=>activeView=v};')(
    {now:()=>now},fn=>{frames.set(++id,fn);return id},id=>frames.delete(id),document,
    ()=>owner,(slot,label)=>labels.push(slot+' '+label),()=>weight,
    (v,n)=>(v/1000).toFixed(n),()=>{},
    {matchMedia:()=>({matches:reducedMotion})});
  const card={valid:true,live:true,scaleAvailable:false,elapsedMs:1000};
  const frame={boot:1,cycle:1,phase:'active',card};
  const tick=()=>{const [id,fn]=frames.entries().next().value;frames.delete(id);fn()};
  timer.sync(frame);timer.schedule();
  now=500;tick();
  assert.equal(labels.at(-1),'Weight 1.5 s','Timer must advance without another message');
  timer.sync({...frame,card:{...card,elapsedMs:1300}});
  now=600;tick();
  assert.equal(timer.elapsed(),1600,'A late message must not slow the clock');
  timer.sync({...frame,card:{...card,elapsedMs:1900}});
  now=700;tick();
  assert.equal(timer.elapsed(),1710,'An earlier-arriving message must correct gradually');
  now=2100;tick();
  assert(timer.stale(),'Silence must freeze the timer after 1.5 seconds');
  const frozen=timer.elapsed();now=4000;
  assert.equal(timer.elapsed(),frozen,'Frozen timer must not advance indefinitely');
  timer.sync({...frame,card:{...card,elapsedMs:4500}});timer.setStale(false);timer.schedule();
  assert.equal(timer.elapsed(),4500,'Recovery must reanchor to firmware time');
  timer.setStale(true);now=4100;tick();
  assert.equal(timer.elapsed(),4500,'Explicit disconnect must freeze immediately');
  timer.sync({...frame,card:{...card,elapsedMs:4600}});timer.setStale(false);timer.schedule();
  document.hidden=true;tick();assert.equal(frames.size,0,'Hidden page must stop animation');
  document.hidden=false;timer.schedule();timer.view('stats');tick();
  assert.equal(frames.size,0,'Leaving Home must stop animation');
  timer.view('home');timer.schedule();owner=false;tick();
  assert.equal(frames.size,0,'Losing UI ownership must stop animation');
  owner=true;
  timer.sync({...frame,cycle:2,card:{...card,elapsedMs:100}});
  assert.equal(timer.elapsed(),100,'New cycle must reset the timer');
  timer.sync({...frame,boot:2,card:{...card,elapsedMs:200}});
  assert.equal(timer.elapsed(),200,'New boot must reset the timer');
  timer.schedule();
  timer.sync({...frame,phase:'pending',card:{...card,live:false,elapsedMs:195}});
  assert.equal(timer.clock(),null,'End must use exact firmware duration rather than local estimate');
  assert.equal(frames.size,0,'End must cancel animation through the retained window');
  timer.sync({...frame,card:{...card,scaleAvailable:true}});timer.schedule();
  assert.equal(timer.clock().id,'Elapsed','Scale shots must run the local clock in the time slot');
  now=4200;tick();
  assert.equal(labels.at(-1),'Elapsed 1.1 s');
  timer.sync({...frame,phase:'pending',card:{...card,scaleAvailable:true,live:false}});
  assert.equal(timer.elapsed(),1000,'Unanimated final values must come from the firmware card');
  weight.animate=(keyframes,options)=>{
    const a={keyframes,options,playState:'running',finished:{then(fn){a.complete=()=>{a.playState='finished';fn()}}},
      cancel(){a.playState='idle';a.cancelled=true}};
    animations.push(a);return a;
  };
  const brew={...frame,cycle:10,card:{...card,elapsedMs:2000}};
  now=5000;timer.sync(brew);timer.schedule();now=5100;tick();
  timer.finish();
  assert.equal(timer.clock(),null,'First Home stop signal must freeze the local clock');
  assert.equal(timer.elapsed(),2100);
  assert.equal(animations.length,0,'The correction must wait for the final firmware frame');
  const ended={...brew,phase:'pending',card:{...brew.card,live:false,elapsedMs:1800}};
  timer.sync(ended);
  assert.equal(animations[0].options.duration,500);
  assert.deepEqual(animations[0].keyframes,[{opacity:1},{opacity:0}]);
  assert.equal(timer.elapsed(),2100,'Final duration must remain hidden until fade out completes');
  animations[0].complete();
  assert.equal(timer.elapsed(),1800,'Swap to authoritative time at zero opacity');
  assert.equal(animations[1].options.duration,500);
  assert.deepEqual(animations[1].keyframes,[{opacity:0},{opacity:1}]);
  animations[1].complete();assert.equal(timer.ending(),null);
  timer.sync(ended);assert.equal(animations.length,2,'Retained updates must not repeat the fade');
  for(const [cycle,final] of [[14,2000],[15,2149],[16,1951]]){
    timer.sync({...brew,cycle});timer.finish();
    timer.sync({...ended,cycle,card:{...ended.card,elapsedMs:final}});
    assert.equal(timer.ending(),null,'A final duration within 0.1 s must swap without a fade');
    assert.equal(timer.elapsed(),final);
  }
  assert.equal(animations.length,2);
  timer.sync({...brew,cycle:11});timer.finish();
  timer.sync({...ended,cycle:11,card:{...ended.card,elapsedMs:2160}});
  assert.equal(animations.length,3,'A 0.2 s correction must fade');
  timer.sync({...brew,cycle:12});
  assert(animations[2].cancelled,'A new cycle must cancel the old fade');
  assert.equal(timer.elapsed(),2000);
  reducedMotion=true;timer.sync({...ended,cycle:12});
  assert.equal(timer.ending(),null,'Reduced motion must skip the transition');
  assert.equal(animations.length,3);
  assert.equal(timer.elapsed(),1800);
  reducedMotion=false;timer.sync({...brew,cycle:13});timer.finish();
  timer.view('stats');timer.sync({...ended,cycle:13});
  assert.equal(timer.ending(),null,'Leaving Home must cancel a pending reveal');
  assert.equal(animations.length,3);
}

{
  const assert=require('assert').strict,{spawnSync}=require('child_process');
  const stream=fs.readFileSync(path.join(sketchDir,'network/OpenBrewByWeightUiStream.inc'),'utf8');
  const wifi=fs.readFileSync(path.join(sketchDir,'network/OpenBrewByWeightWifi.inc'),'utf8');
  const cadence=stream.slice(stream.indexOf('constexpr uint32_t kUiStreamSampleStride'),
    stream.indexOf('constexpr size_t kUiStreamRequestBytes'));
  const service=stream.slice(stream.indexOf('void OpenBrewByWeightNetwork::noteScaleSample('),
    stream.indexOf('void OpenBrewByWeightNetwork::uiStreamDispatch('));
  const sync=wifi.slice(wifi.indexOf('void OpenBrewByWeightNetwork::syncControlCriticalRf('),
    wifi.indexOf('void OpenBrewByWeightNetwork::syncScaleHuntRf('));
  const directory=path.resolve(sketchDir,'..','temp','ai_temp_no_scale_stop_fade');
  fs.mkdirSync(directory,{recursive:true});
  const binary=path.join(directory,'dispatch-'+process.pid);
  const native=`
#include <atomic>
#include <cassert>
#include <cstdint>
constexpr int ESP_OK=0;
${cadence}
int queued=0,queueResult=ESP_OK,notified=0;
int httpd_queue_work(int,void (*)(void *),void *){++queued;return queueResult;}
void xTaskNotifyGive(void *){++notified;}
struct TaskLockGuard { explicit TaskLockGuard(int &){} };
struct OpenBrewByWeightNetwork {
  int server_=1,dataMux_=0;
  void *taskHandle_=this;
  struct {int fd=-1;} uiStreams_[2];
  struct {void setControlCritical(bool){}} webhooks_;
  std::atomic<bool> uiStreamWorkPending_{false},uiStreamUrgent_{false},
    controlCriticalRfActive_{false},ntpCallbackAccepting_{true},ntpAbortRequested_{false};
  std::atomic<uint32_t> rfGateGeneration_{0},scaleSamples_{0},uiStreamDispatchSamples_{0};
  std::atomic<bool> uiStreamOpen_{false};
  uint32_t uiStreamDispatchAtMs_=100;
  static void uiStreamDispatch(void *){}
  void noteScaleSample();
  void serviceUiStream(uint32_t);
  void syncControlCriticalRf(bool,bool=false);
};
${service}
${sync}
int main(){
  OpenBrewByWeightNetwork n;n.uiStreams_[0].fd=4;
  n.serviceUiStream(150);assert(queued==0);
  n.syncControlCriticalRf(true);assert(notified==1);
  n.serviceUiStream(151);assert(queued==1&&!n.uiStreamUrgent_);
  n.syncControlCriticalRf(false);
  n.serviceUiStream(152);assert(queued==1&&n.uiStreamUrgent_);
  n.uiStreamWorkPending_=false;
  n.serviceUiStream(153);assert(queued==2&&!n.uiStreamUrgent_);
  n.uiStreamWorkPending_=false;n.serviceUiStream(200);assert(queued==2);
  n.syncControlCriticalRf(true);queueResult=-1;
  n.serviceUiStream(201);assert(queued==3&&n.uiStreamUrgent_&&!n.uiStreamWorkPending_);
  queueResult=ESP_OK;n.serviceUiStream(202);assert(queued==4&&!n.uiStreamUrgent_);
  n.uiStreamWorkPending_=false;n.uiStreams_[0].fd=-1;
  n.syncControlCriticalRf(false);n.serviceUiStream(203);assert(queued==4&&n.uiStreamUrgent_);
  n.uiStreams_[0].fd=4;n.serviceUiStream(204);assert(queued==5);
  n.uiStreamWorkPending_=false;
  n.syncControlCriticalRf(false,true);n.serviceUiStream(205);assert(queued==6);
  n.uiStreamWorkPending_=false;n.syncControlCriticalRf(true);n.serviceUiStream(206);
  assert(queued==7);n.uiStreamWorkPending_=false;
  n.syncControlCriticalRf(true,true);n.serviceUiStream(207);assert(queued==8);
  // Periodic frames follow every second scale sample; the timer covers silence.
  n.uiStreamWorkPending_=false;n.uiStreamOpen_=true;notified=0;
  n.noteScaleSample();n.serviceUiStream(300);assert(queued==8&&notified==0);
  n.noteScaleSample();assert(notified==1);
  n.serviceUiStream(356);assert(queued==8);
  n.serviceUiStream(357);assert(queued==9);
  n.uiStreamWorkPending_=false;n.noteScaleSample();n.noteScaleSample();
  assert(notified==2);n.serviceUiStream(506);assert(queued==9);
  n.serviceUiStream(507);assert(queued==10);
  n.uiStreamWorkPending_=false;n.serviceUiStream(806);assert(queued==10);
  n.serviceUiStream(807);assert(queued==11);
  // An in-flight dispatch or a closed stream never wakes the network task.
  n.noteScaleSample();n.noteScaleSample();assert(notified==2);
  n.uiStreamWorkPending_=false;n.uiStreamOpen_=false;n.noteScaleSample();assert(notified==2);
  // An urgent dispatch restarts the sample window.
  n.uiStreamOpen_=true;n.uiStreamUrgent_=true;n.serviceUiStream(900);assert(queued==12);
  n.uiStreamWorkPending_=false;n.noteScaleSample();assert(notified==2);
  n.serviceUiStream(1050);assert(queued==12);
  n.noteScaleSample();assert(notified==3);n.serviceUiStream(1050);assert(queued==13);
}
`;
  try{
    const compiled=spawnSync(process.env.CXX||'c++',['-std=c++17','-Wall','-Wextra','-Werror','-x','c++','-','-o',binary],{input:native,encoding:'utf8'});
    assert.equal(compiled.status,0,compiled.error?.message||compiled.stderr);
    const run=spawnSync(binary,[],{encoding:'utf8'});
    assert.equal(run.status,0,run.error?.message||run.stderr);
  }finally{fs.rmSync(binary,{force:true})}
  const lockFailure=stream.slice(stream.indexOf('if (xSemaphoreTake(self.statusResponseMux_'),
    stream.indexOf('self.callbacks_.refreshControlStatus();'));
  assert(lockFailure.includes('uiStreamUrgent_.store(true'),'Workspace contention must preserve urgent delivery');
  assert(stream.includes('(control.activeCycle || control.relayClosed) !='),
    'A notification preceding the committed snapshot must preserve urgent delivery');
  assert(firmwareCore.includes('syncControlCriticalRf(next.activeCycle || next.relayClosed, true)'),
    'Every changed control gate, including activator edges, must request urgent delivery');
  assert(network.includes('delta.field("physicalActivatorOn", bool(control.physicalActivatorOn))'),
    'Home must include physical activator edges even when the cycle state does not change');
  assert(codeIncludes(runtimeJs, "homeFrame.status.machineType==='paddle'&&paddleOff"),
    'Only paddle OFF, never a momentary button release, may trigger an early timer fade');
  const edgeSource=/const\s+paddleOff\s*=([\s\S]*?);/.exec(runtimeJs)[1];
  const paddleOff=new Function('homeFrame','data','return ('+edgeSource+')');
  assert(paddleOff({status:{physicalActivatorOn:true}},{changes:{physicalActivatorOn:false}}));
  assert(!paddleOff({status:{physicalActivatorOn:false}},{changes:{physicalActivatorOn:false}}),
    'A remote start with the paddle already OFF must keep counting');
  assert(!paddleOff({status:{physicalActivatorOn:true}},{changes:{'cycle.active':true}}));
  assert(!paddleOff(null,{changes:{physicalActivatorOn:false}}));
}

if (!statusSection || !statusSection[1].includes('class="lamp"') ||
    statusSection[1].includes('class="statusColumn"') ||
    statusSection[1].includes('class="row"') ||
    (statusSection[1].match(/class="metric[ "]/g) || []).length !== 6 ||
    !statusSection[1].includes('id="machineRowState"') ||
    !statusSection[1].includes('id="machineStateValue"') ||
    !statusSection[1].includes('<strong>Activity</strong>') ||
    !statusSection[1].includes('<strong>Brew</strong>') ||
    statusSection[1].includes('<strong>Cup</strong>') ||
    statusSection[1].includes('data-label="Machine"') ||
    !statusSection[1].includes('id="machineState"') ||
    !statusSection[1].includes('id="state"') ||
    !statusSection[1].includes('id="homeMicraPower"') ||
    !statusSection[1].includes('id="homeMicraBrewBoiler"') ||
    !statusSection[1].includes('id="homeMicraSteamBoiler"') ||
    statusSection[1].includes('id="cupState"') ||
    statusSection[1].includes('id="paddle"') ||
    statusSection[1].includes('id="relay"') ||
    statusSection[1].includes('id="safety"') ||
    statusSection[1].includes('id="statusExtractionGuard"') ||
    !scaleSection || !scaleSection[1].includes('class="lampState"') ||
    (scaleSection[1].match(/class="metric"/g) || []).length !== 3 ||
    !scaleSection[1].includes('<strong>My scale</strong>') ||
    !scaleSection[1].includes('<strong>Weight</strong>') ||
    !scaleSection[1].includes('<strong>Timer</strong>') ||
    scaleSection[1].includes('data-label="Status"') ||
    !scaleSection[1].includes('id="scale"') ||
    !scaleSection[1].includes('id="preferredScale"') ||
    !scaleSection[1].includes('id="scaleWeight"') ||
    !scaleSection[1].includes('id="scaleTimer"') ||
    !codeIncludes(ui, 's.physicalActivatorOn?') ||
    !codeIncludes(ui, 's.relayClosed?') || !codeIncludes(ui, 'ON') || !codeIncludes(ui, 'OFF') ||
    !codeIncludes(ui, 'function formatScaleWeight(') ||
    !codeIncludes(ui, 'function formatScaleStatus(') ||
    !codeIncludes(ui, 'function formatScaleTimer(') ||
    !codeIncludes(ui, 'function formatMachineState(') ||
    !codeIncludes(ui, 'IDLE:') || !codeIncludes(ui, 'Idle') ||
    !codeIncludes(ui, 'RUNNING_UNCONFIRMED:') || !codeIncludes(ui, 'Starting') ||
    !codeIncludes(ui, 'RUNNING:') || !codeIncludes(ui, 'Running') ||
    !codeIncludes(ui, 'IDLE_UNCONFIRMED:') || !codeIncludes(ui, 'Stopping') ||
    !codeIncludes(ui, 'function formatCupState(') ||
    !codeIncludes(ui, 'lastDisconnectReasonName') ||
    !codeIncludes(ui, 'Stale') || !codeIncludes(ui, 'No sample') ||
    !codeIncludes(ui, 'formatScaleStatus(s)') ||
    !codeIncludes(ui, 'id="preferredScale"') ||
    !codeIncludes(ui, 'id="preferredScaleSelect"') ||
    !codeIncludes(ui, 'id="preferredScalePauseHint"') ||
    !codeIncludes(ui, 'id="preferredScaleBootstrapHint"') ||
    !codeIncludes(ui, 'id="scalePreference"') ||
    !codeIncludes(ui, 'id="forgetPairedScale"') ||
    !codeIncludes(ui, 'Which scale') ||
    !codeIncludes(ui, 'Any scale') ||
    !codeIncludes(ui, 'First detected') ||
    !codeIncludes(ui, 'My scale first') ||
    !codeIncludes(ui, 'Only my scale') ||
    !codeIncludes(ui, 'My scale') ||
    !codeIncludes(ui, 'Forget scale') ||
    !codeIncludes(ui, 'scaleMacCacheMode') ||
    !codeIncludes(ui, '/api/v1/scale/preferred/clear') ||
    codeIncludes(ui, "command('/api/v1/scale/preferred/select'") ||
    !codeIncludes(ui, 'function formatPreferredScale(') ||
    !codeIncludes(ui, 'function updatePreferredScaleSelect(') ||
    !codeIncludes(ui, 'function updateScalePreferenceOptions(') ||
    !codeIncludes(ui, "if(!preferred||(keep&&!prev)){const first=document.createElement('option')") ||
    !codeIncludes(ui, "first.textContent=mode==='only'||mode==='prefer'?") ||
    !codeIncludes(ui, 'empty.textContent=bootstrap?') ||
    !codeIncludes(ui, 'First detected') || !codeIncludes(ui, 'None saved') ||
    codeIncludes(ui, "msg:'Select a preferred scale first.'") ||
    !codeIncludes(ui, '<option value="prefer" selected>My scale first</option>') ||
    codeIncludes(ui, '<option value="first" selected>Any scale</option>') ||
    codeIncludes(ui, '<option value="only" selected>Only my scale</option>') ||
    codeIncludes(ui, "if(!canPrefer&&(sel.value==='prefer'||sel.value==='only'))") ||
    codeIncludes(ui, "o.disabled=!canPrefer||!controlsMutable") ||
    // Regression: missing ';' after `prev` concatenated into
    // `prevupdateScalePreferenceOptions` and broke Settings status refresh.
    codeIncludes(ui, ':prevupdateScalePreferenceOptions') ||
    !codeIncludes(ui, "sel.value=keep?prev:preferred||'';updateScalePreferenceOptions()") ||
    !codeIncludes(ui, "sel.dataset.pending='1'") ||
    !codeIncludes(ui, "scaleMacCacheMode:['first','prefer','only'].includes($('scalePreference')?.value)?$('scalePreference').value:'only'") ||
    !codeIncludes(ui, "preferredScaleMac:$('preferredScaleSelect')?.value") ||
    codeIncludes(ui, 'id="alwaysUseThisScale"') ||
    codeIncludes(ui, 'Always use this scale') ||
    !codeIncludes(ui, 'function selectPreferredScale(') ||
    !codeIncludes(ui, 'function forgetPairedScale(') ||
    !codeIncludes(ui, 'Saved scales stay in the list') ||
    !codeIncludes(ui, 'formatPreferredScale(s)') ||
    !codeIncludes(ui, 'macCachePauseRemainingMs>0') ||
    codeIncludes(ui, 'id="preferredScaleSettings"') ||
    codeIncludes(ui, 'id="scaleMacCacheMode"') ||
    codeIncludes(ui, 'id="clearPreferredScale"') ||
    codeIncludes(ui, 'id="scaleMacCacheFullWarn"') ||
    codeIncludes(ui, 'Use scale MAC cache') ||
    codeIncludes(ui, 'Paired scale') ||
    codeIncludes(ui, 'Forget this scale') ||
    !network.includes('preferredScaleClearHandler') ||
    !network.includes('preferredScaleSelectHandler') ||
    !network.includes('/api/v1/scale/preferred/clear') ||
    !network.includes('/api/v1/scale/preferred/select') ||
    !network.includes('Scale preference must be first, prefer, or only.') ||
    network.includes('Always use this scale must be on or off.') ||
    network.includes('scaleMacCacheMode must be disabled or full.') ||
    !network.includes('scaleMacCacheMode must be first, prefer, or only.') ||
    firmwareCore.includes('scaleMacCacheModeRequiresPreferred(candidate.scaleMacCacheMode)') ||
    scaleWorker.includes('coerceScalePreferenceModeToFirst') ||
    !scaleWorker.includes('First detected scale adopted: %s — %s') ||
    !network.includes('The paired scale cannot be forgotten while a cycle') ||
    !network.includes('\\"history\\"') ||
    network.includes('Preferred scale cache cannot be cleared') ||
    !network.includes('\\"timerMs\\"')) {
  throw new Error('Home Status must show Activity/Brew/Cup and a Scale panel with one value per label');
}
if (!codeIncludes(ui, 'id="renameScaleLink"') ||
    !codeIncludes(ui, 'id="preferredScaleRenameWrap"') ||
    !codeIncludes(ui, 'id="preferredScaleSelectRenameWrap"') ||
    !codeIncludes(ui, 'id="dScaleName"') ||
    !codeIncludes(ui, 'id="dScaleNameRename"') ||
    !codeIncludes(ui, 'id="dScaleNameRenameWrap"') ||
    !codeIncludes(ui, 'Connected scale') ||
    !codeIncludes(ui, '(rename)') ||
    !codeIncludes(ui, 'function renameScale(') ||
    !codeIncludes(ui, 'function updateScaleRenameUi(') ||
    !codeIncludes(ui, 'function scaleDisplayName(') ||
    !codeIncludes(ui, 'function validScaleFriendlyNameClient(') ||
    !codeIncludes(ui, '/api/v1/scale/friendly-name') ||
    !codeIncludes(ui, 'preferredFriendlyName') ||
    !codeIncludes(ui, 'Name this scale') ||
    !codeIncludes(ui, 'e.friendlyName&&String(e.friendlyName).trim()') ||
    !codeIncludes(ui, "o.dataset.name=(e.name&&String(e.name).trim())||''") ||
    !codeIncludes(ui, "opt.dataset?String(opt.dataset.name||'')") ||
    !network.includes('scaleFriendlyNameHandler') ||
    !network.includes('/api/v1/scale/friendly-name') ||
    !network.includes('UNKNOWN_SCALE') ||
    !network.includes('The scale name cannot be changed while a cycle') ||
    !network.includes('connectedFriendlyName') ||
    !network.includes('connectedMac') ||
    !codeIncludes(viewJs.diagnostic, "R.updateScaleRenameUi('dScaleNameRenameWrap',sc.connectedMac||'',sc.connectedFriendlyName||''") ||
    !network.includes('delta.field("scale.preferredFriendlyName"') ||
    !scaleWorker.includes('bool setScaleFriendlyName(') ||
    codeIncludes(ui, "label.split(' — ')")) {
  throw new Error(
      'Scale names must support a friendly-name override with rename links in Home and Diagnostic');
}
{
  const wrap = {hidden:false, classList:{toggle(_, hidden){wrap.hidden=hidden}},
    querySelector(){return {classList:{toggle(){}},setAttribute(){}}}};
  const helpers = runtimeJs.slice(runtimeJs.indexOf('function scaleDisplayName('),
      runtimeJs.indexOf('function validScaleFriendlyNameClient('));
  const {update, selected} = new Function('$', 'controlsMutable', helpers +
      ';return {update:updateScaleRenameUi,selected:()=>scaleRename}')(id => wrap, true);
  update('dScaleNameRenameWrap', 'AA:BB:CC:DD:EE:02', 'Connected');
  if (selected().mac !== 'AA:BB:CC:DD:EE:02' || selected().current !== 'Connected' || wrap.hidden)
    throw new Error('Diagnostic rename must target the connected scale');
  update('dScaleNameRenameWrap', '', '');
  if (selected().mac || !wrap.hidden)
    throw new Error('Diagnostic rename must disappear when no scale is connected');
}
if (codeIncludes(ui, 'id="shotPanel"') ||
    codeIncludes(ui, 'id="shotBar"') ||
    codeIncludes(ui, 'id="shotBarFast"') ||
    codeIncludes(ui, 'id="shotBarTicks"') ||
    partialHtml.home.includes('id="shotBarTicks"') ||
    partialHtml.home.includes('<legend>Current / Last Shot</legend>') ||
    partialHtml.home.includes('class="ruleChartLabel">Weight (g)</div>') ||
    partialHtml.home.includes('id="shotIdle"') ||
    css.includes('content:"Weight (g)"') ||
    css.includes('#shotIdle') ||
    codeIncludes(ui, 'shotMark') ||
    !codeIncludes(ui, 'cutLine(v, m)') ||
    css.includes('.shotTrack') ||
    css.includes('#shotBarTicks') ||
    css.includes('.shotMark') ||
    css.includes('max-width:150%') ||
    codeIncludes(ui, 'id="shotCard"') ||
    html.includes('id="shotSparkHost"') ||
    !css.includes('.shotSparkHost') ||
    !css.includes('.shotSpark{') ||
    !css.includes('.shotSparkY{') ||
    !css.includes('.shotSparkHost .ruleChartTicks') ||
    !css.includes('.hidden,[hidden]{display:none!important}') ||
    css.includes('#shotPanel') ||
    !css.includes('#shotTable td.shotSparkCell{grid-area:spark;display:grid;gap:.65rem') ||
    !css.includes('#shotTable tr.noSpark{') ||
    !css.includes('.shotSpark{grid-area:plot;display:block;width:100%;height:100%;color:var(--ok);overflow:visible}') ||
    !css.includes('.shotGrid,.rvGrid{stroke:var(--ln);stroke-width:.8;opacity:.7}') ||
    !css.includes('.shotGrid,.shotTrace{vector-effect:non-scaling-stroke}') ||
    !css.includes('grid-template-rows:var(--shot-plot-h,max(7rem,var(--shot-plot-min,0rem))) auto') ||
    !css.includes('grid-template-areas:"y plot f" ". x ."') ||
    css.includes('.shotEventTicks') ||
    !css.includes('.shotYTick{position:absolute;right:0;white-space:nowrap;transform:translateY(-50%)}') ||
    css.includes('.shotYTick:first-child') || css.includes('.shotYTick:last-child') ||
    css.includes('.ruleChartLabel') || !css.includes('.shotLegDrip i{') ||
    !codeIncludes(ui, 'function renderShotSpark(') ||
    !codeIncludes(statsRuntimeJs, 'function buildShotSparkModel(') ||
    !codeIncludes(statsRuntimeJs, 'function axisLabel(') ||
    !codeIncludes(statsRuntimeJs, 'function fillChartTicks(') ||
    codeIncludes(statsRuntimeJs, 's[a=') ||
    !codeIncludes(statsRuntimeJs, 'style.left=') ||
    !codeIncludes(statsRuntimeJs, "style.setProperty('--shot-plot-min'") ||
    !codeIncludes(statsRuntimeJs, '.style.top=') ||
    // The firmware CSP (style-src 'self') drops style attributes parsed from markup.
    codeIncludes(statsRuntimeJs, 'style="') ||
    !codeIncludes(statsRuntimeJs, 'function shotDisplayFlowGS(') ||
    !codeIncludes(statsRuntimeJs, 'if(!pts.length||dur<=0)return null') ||
    !codeIncludes(statsRuntimeJs, 'm.firstDropS>0') ||
    !codeIncludes(statsRuntimeJs, 'series(m.flowSegs,m.dripFlowSegs,step*n,true)') ||
    !codeIncludes(statsRuntimeJs, 'Flow rate (g/s)') ||
    !codeIncludes(statsRuntimeJs, "querySelector('.ruleChartTicks')") ||
    codeIncludes(statsRuntimeJs, "['.ruleChartTicks',xt]") || codeIncludes(statsRuntimeJs, "'.shotEventTicks'") ||
    codeIncludes(statsRuntimeJs, "'1st '+L(") ||
    codeIncludes(statsRuntimeJs, 'fillChartTicks($(\'shotBarTicks\')') ||
    !codeIncludes(statsRuntimeJs, 'raw.sort(') ||
    codeIncludes(statsRuntimeJs, 'shotIdle') ||
    codeIncludes(statsRuntimeJs, "last?'Last shot.'") ||
    css.includes('.shotFirstDrop') ||
    codeIncludes(statsRuntimeJs, 'stroke="currentColor"') ||
    !codeIncludes(statsRuntimeJs, "if(spark.hidden)row.classList.add('noSpark')") ||
    !css.includes('.shotCard{') ||
    !css.includes('.metric,.shotCard>*{') ||
    !css.includes('.metric strong,.shotCard strong{') ||
    !css.includes('.metric>div,.shotCard>*>div{') ||
    css.includes('#diagnosticsPanel .metric,#statusPanel .metric,#scalePanel .metric,.shotCard>*{') ||
    css.includes('#statusPanel .metric::before,#scalePanel .metric::before,.shotCard>*::before{') ||
    css.includes('font-size:1rem;font-weight:700;color:var(--mu)') ||
    !css.includes('.shotCard .shotDur>div,.shotCard .shotActual>div') ||
    !css.includes('grid-template-areas:"dur dur dur actual actual actual" "goal goal err err avgflow avgflow"') ||
    css.includes('grid-template-areas:"dur dur dur actual actual actual" "goal goal avgflow avgflow maxflow maxflow" "err err tare tare drop drop" "ended ended shot shot preset preset" "scale scale rate rate rate rate"') ||
    codeIncludes(ui, 'id="shotElapsed"') ||
    codeIncludes(ui, 'id="shotMoment"') ||
    codeIncludes(ui, "formatHumanTime(d.momentSec)") ||
    codeIncludes(ui, 'id="shotFirstDrop"') ||
    codeIncludes(ui, 'id="shotTareTime"') ||
    codeIncludes(ui, 'id="shotScale"') ||
    codeIncludes(ui, 'id="shotCurrentWeight"') ||
    partialHtml.home.includes('<strong>Yield</strong>') ||
    partialHtml.home.includes('<strong>Avg flow</strong>') ||
    partialHtml.home.includes('<strong>Max flow</strong>') ||
    partialHtml.home.includes('<strong>Dur</strong>') ||
    partialHtml.home.includes('data-label=') ||
    html.includes('data-label="Actual"') ||
    codeIncludes(ui, 'id="shotGoalWeight"') ||
    codeIncludes(ui, 'id="shotErr"') ||
    codeIncludes(ui, 'id="shotFlow"') ||
    codeIncludes(ui, 'id="shotMaxFlow"') ||
    codeIncludes(ui, 'id="shotEnded"') ||
    codeIncludes(ui, 'id="shotType"') ||
    codeIncludes(ui, 'id="shotPreset"') ||
    !codeIncludes(ui, 'shotFrame.card') ||
    codeIncludes(ui, 'id="shotRetare"') ||
    codeIncludes(ui, 'id="shotGuard"') ||
    codeIncludes(ui, 'id="shotPct"') ||
    !codeIncludes(ui, 'id="shotHero"') ||
    !codeIncludes(ui, 'shotHeroState') ||
    !codeIncludes(ui, 'function uiStreamFrame(') ||
    !network.includes('firstDropElapsedMs') ||
    !network.includes('shotType') ||
    !network.includes('scaleProtocol') ||
    !codeIncludes(ui, 'remoteReady&&relayStartReady&&canControl') ||
    codeIncludes(ui, 'Remote machine control disabled by policy') ||
    !network.includes('delta.field("remoteControlEnabled"') ||
    !network.includes('delta.field("lastCommand.requestId"') ||
    !network.includes('delta.field("maintenance.active"') ||
    !network.includes('delta.field("maintenance.persistPending"') ||
    !network.includes('delta.field("maintenance.persistFailed"') ||
    !codeIncludes(ui, 'persistFailed') ||
    !codeIncludes(ui, 'Saving…') ||
    !network.includes('delta.field("cycle.active"') ||
    !network.includes('extractionExtended') ||
    !codeIncludes(ui, 'paintUiStream()')) {
  throw new Error('Web UI must enforce remote policy, maintenance, durable command state, and live shot status');
}
{
  const curveTypes = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightShotCurveTypes.h'), 'utf8');
  const record = curveTypes.slice(curveTypes.indexOf('struct ShotCurveRecord'),
      curveTypes.indexOf('inline ShotCurveRecord emptyShotCurveRecord'));
  if (!codeIncludes(statsRuntimeJs, "spark.className='shotSparkCell'") ||
      !codeIncludes(statsRuntimeJs, "renderShotSpark(spark,r,m)") ||
      /flow/i.test(record) || network.includes('\"flowCg\"') || network.includes('\"flowDtS\"')) {
    throw new Error('Stats must share the ordered Weight/Flow chart renderer without a persisted flow series');
  }
}
if (!codeIncludes(ui, 'id="autoToManualGuardEnabled"') ||
    !codeIncludes(ui, 'id="scaleLossFixedS"') ||
    !codeIncludes(ui, 'id="scaleTimerStopExtraDelayMs"') ||
    !html.includes('Delays only the scale') ||
    html.includes('Added after measured scale start lag') ||
    html.includes('Added after the scale timer catches up to circuit whole seconds') ||
    !codeIncludes(ui, 'id="dripDelayS" type="number" min="0" max="10" step="0.1"') ||
    !codeIncludes(ui, 'id="scaleLossFixedLink"') ||
    html.indexOf('<summary>Scale guard</summary>') >
        html.indexOf('<summary>Finish by time</summary>') ||
    html.indexOf('<summary>Finish by time</summary>') >
        html.indexOf('id="scaleLossProjection"') ||
    !codeIncludes(ui, 'id="noScaleBbwMode"') ||
    !codeIncludes(ui, 'id="noScaleAllowRinseWhileArmed"') ||
    !codeIncludes(ui, 'id="lastShotCooldownMin"') ||
    !codeIncludes(ui, 'When no scale') ||
    !codeIncludes(ui, 'Warning returns after') ||
    !codeIncludes(ui, 'Rinse without a scale') ||
    !codeIncludes(ui, "noScaleAllowRinseWhileArmed:$('noScaleAllowRinseWhileArmed').checked") ||
    !codeIncludes(ui, "'rinseEnabled','noScaleAllowRinseWhileArmed'") ||
    !codeIncludes(ui, 'id="noScaleBbwMode"') ||
    !codeIncludes(ui, 'value="warn_once"') ||
    !codeIncludes(ui, 'value="require_scale"') ||
    !codeIncludes(ui, 'function formatNoScaleGuard(') ||
    !codeIncludes(ui, 'function formatSlowExtractionGuard(') ||
    codeIncludes(ui, 'function updateNoScaleGuard(') ||
    html.includes('id="shotAtmGuard"') ||
    html.includes('id="shotNoScaleGuard"') ||
    html.includes('id="statusExtractionGuard"') ||
    html.includes('id="statusSlowExtractionGuard"') ||
    html.includes('id="statusAtmGuard"') ||
    html.includes('id="statusNoScaleGuard"') ||
    html.indexOf('<legend>Shared settings ') >
        html.indexOf('<summary>Paddle</summary>') ||
    html.indexOf('<summary>Paddle</summary>') >
        html.indexOf('<summary>Scale guard</summary>') ||
    html.indexOf('<summary>Quick rinse</summary>') >
        html.indexOf('<summary>Scale guard</summary>') ||
    html.indexOf('id="noScaleBbwMode"') <
        html.indexOf('<legend>Shared settings ') ||
    html.indexOf('id="noScaleBbwMode"') >
        html.indexOf('id="noScaleAllowRinseWhileArmed"') ||
    html.indexOf('id="noScaleAllowRinseWhileArmed"') >
        html.indexOf('id="lastShotCooldownMin"') ||
    html.indexOf('id="lastShotCooldownMin"') <
        html.indexOf('<summary>Scale guard</summary>') ||
    html.indexOf('id="lastShotCooldownMin"') >
        html.indexOf('<summary>Finish by time</summary>') ||
    html.indexOf('id="paddleMode"') <
        html.indexOf('<summary>Paddle</summary>') ||
    html.indexOf('id="paddleMode"') >
        html.indexOf('<summary>Scale guard</summary>') ||
    html.indexOf('id="noScaleBbwMode"') <
        html.indexOf('id="brewConflictChip"') ||
    !network.includes('noScaleBbwMode') ||
    !network.includes('noScaleAllowRinseWhileArmed') ||
    !network.includes('warn_once') ||
    !network.includes('require_scale') ||
    !network.includes('lastShotCooldownMs') ||
    !network.includes('serialLogLevel') ||
    !network.includes('ringRetainLogLevel') ||
    !network.includes('noScaleShotGuard') ||
    !network.includes('noScaleShotGuardEnabled') ||
    !network.includes('noScaleShotGuardArmed') ||
    !network.includes('cupPresence') ||
    !network.includes('control.cupPresent') ||
    !network.includes('machineState') ||
    !network.includes('machineRunStateName') ||
    !network.includes('cupPresenceStateName') ||
    !network.includes('cupPresenceStateName(control.cupPresenceState) : "UNKNOWN"') ||
    !firmware.includes('last.noScaleShotGuardEnabled') ||
    !firmware.includes('last.noScaleShotGuardArmed') ||
    !firmware.includes('next.cupPresent') ||
    !firmware.includes('next.machineRunState') ||
    !firmware.includes('next.cupPresenceState') ||
    !firmware.includes('cupPresenceState() == CupPresenceState::PRESENT') ||
    !domain.includes('bool cupPresent = false') ||
    !domain.includes('MachineRunState machineRunState') ||
    !domain.includes('CupPresenceState cupPresenceState') ||
    !codeIncludes(ui, 'Scale lost ·') ||
    codeIncludes(ui, "setHomeSub('homeBbwSub'") ||
    codeIncludes(ui, "setHomeSub('homeAlertsSub'") ||
    !codeIncludes(ui, 'function formatCupProtection(') ||
    codeIncludes(ui, 'function formatAlertsChannel(') ||
    !/Place a cup to start/.test(ui) ||
    !codeIncludes(ui, 'Shot aborted') ||
    !codeIncludes(ui, 'Brew allowed') ||
    !codeIncludes(ui, "c.state==='PRESENT'||c.present?") ||
    !codeIncludes(ui, 'Present') || !codeIncludes(ui, 'Absent') ||
    !network.includes('shotLogStopDetailName(') ||
    !html.includes('option value="scale_priority">Scale first') ||
    !codeIncludes(ui, 'yield_source') ||
    !network.includes('autoToManualGuardEnabled') ||
    !network.includes('scaleLossFixedS') ||
    !network.includes('scaleTimerStopExtraDelayMs') ||
    !network.includes('dripDelayMs') ||
    !network.includes('autoToManualGuardTrendMs') ||
    !network.includes('autoToManualGuardEnforced') ||
    !network.includes('autoToManualGuardArmed') ||
    !network.includes('actualWeightSource') ||
    !network.includes('reset-guard-samples') ||
    !network.includes('AUTO_TO_MANUAL_GUARD') ||
    !domain.includes('"UNCONFIRMED_START"') ||
    !network.includes('endReasonName(') ||
    !network.includes('cupProtectionEnabled') ||
    !network.includes('stopIfCupRemoved') ||
    !network.includes('requireCupToStart') ||
    !network.includes('cupRemovedWeightG') ||
    !codeIncludes(ui, "cupRemovedWeightG:number('cupRemovedWeightG')")) {
  throw new Error('Auto-to-manual time guard must be wired in config UI, live panel, shots API, and routes');
}
if (!network.includes('callbacks_.copyHomeShot(latest, control.activeCycle || control.homePending') ||
    !network.includes('? nullptr : &self.workBuf_->homeCurve') ||
    !network.includes('callbacks_.copyHomeShot(latestShot, nullptr)') ||
    !network.includes('shotLogProjectLastShot(latest, control.lastShot, control.lastGoodShot)') ||
    !network.includes('shotLogProjectLastShot(latestShot, control.lastShot, control.lastGoodShot)') ||
    network.includes('\"lastGoodShot\":%s') ||
    !firmwareCore.includes('shotLog.copyNewestEligible(record)') ||
    !network.includes('session.cycle = card.cycleId;') ||
    !network.includes('delta.field("presetName", "\\"%s\\"", preset);') ||
    !network.includes('delta.field("averageFlowGps", "%s", flow);') ||
    !network.includes('card.averageFlowValid && std::isfinite(card.averageFlowGps)') ||
    network.includes('scaleFlowGps') ||
    codeIncludes(ui, 'rateLastShotValue') ||
    codeIncludes(ui, 'controlsMutable&&last&&!live&&ls.shotLogId') ||
    codeIncludes(ui, 'clearLastShotButton')) {
  throw new Error('Home and integration must project the newest eligible history row');
}
const paddleHelp = html.slice(html.indexOf('<summary>Paddle</summary>'),
    html.indexOf('</details>', html.indexOf('<summary>Paddle</summary>')));
if (!html.includes('<summary>Paddle</summary>') ||
    !html.includes('id="paddleMode"') ||
    !html.includes('<option value="auto">Auto</option>') ||
    !html.includes('<option value="natural">Natural</option>') ||
    !html.includes('<option value="original">Original</option>') ||
    html.indexOf('<option value="auto">Auto</option>') >
        html.indexOf('<option value="natural">Natural</option>') ||
    html.indexOf('<option value="natural">Natural</option>') >
        html.indexOf('<option value="original">Original</option>') ||
    !paddleHelp.includes('<strong>Natural:</strong>') ||
    !paddleHelp.includes('<strong>Original:</strong>') ||
    !paddleHelp.includes('<strong>Auto:</strong>') ||
    paddleHelp.indexOf('<strong>Natural:</strong>') >
        paddleHelp.indexOf('<strong>Original:</strong>') ||
    paddleHelp.indexOf('<strong>Original:</strong>') >
        paddleHelp.indexOf('<strong>Auto:</strong>') ||
    !html.includes('ON starts, OFF stops') ||
    !html.includes('To stop early, ON then OFF again') ||
    !html.includes('Left ON, it runs to the 60 s safety limit') ||
    !html.includes('wherever the paddle is') ||
    !html.includes('after the first second the paddle') ||
    !html.includes('OFF in the first second stops it, or starts a rinse if Quick rinse is on') ||
    !html.includes('Without weight control, OFF stops normally') ||
    !codeIncludes(ui, "paddleMode:$('paddleMode')?['auto','natural','original']") ||
    !codeIncludes(ui, "if($('paddleMode'))$('paddleMode').value=") ||
    !network.includes('"paddleMode"') ||
    !network.includes('paddleMode must be auto, natural or original.') ||
    !network.includes('jsonPaddleMode') ||
    !firmware.includes('machineApplyWorkflowConfig') ||
    !firmwareCore.includes('machineApplyWorkflowConfig(candidate, command.config)') ||
    !firmware.includes('dst.paddleMode = src.paddleMode') ||
    firmwareCore.includes('candidate.paddleMode') ||
    !firmware.includes('machineHidesPhysicalStop()') ||
    !firmware.includes('machineAllowsAutomationStop()') ||
    !firmware.includes('machineBeginCycle') ||
    domain.includes('enum class PaddleMode') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8')
         .includes('enum class PaddleMode') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8')
         .includes('NATURAL = 0') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8')
         .includes('ORIGINAL = 1') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8')
         .includes('AUTO = 2') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineTypes.h'), 'utf8')
         .includes('enum class PaddleMode') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineTypes.h'), 'utf8')
         .includes('parsePaddleMode') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineTypes.h'), 'utf8')
         .includes('enum class MachineType') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddlePolicy.h'), 'utf8')
         .includes('machinePollIntention()') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8')
        .includes('enum class PaddleMode') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrew.h'), 'utf8')
        .includes('PaddleMode::') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrew.h'), 'utf8')
        .includes('paddleMode')) {
  throw new Error('Machine Paddle mode must expose Auto/Natural/Original in UI, API, and APPLY_CONFIG');
}
const brewHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrew.h'), 'utf8');
const scaleSenseHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightScaleSense.h'), 'utf8');
const paddlePolicyHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachinePaddlePolicy.h'), 'utf8');
const machineTypesHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachineTypes.h'), 'utf8');
const momentaryInputHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryInput.h'), 'utf8');
const momentaryConfigHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryConfig.h'), 'utf8');
const momentaryControlHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryControl.h'), 'utf8');
const momentaryOnlyHeader = fs.readFileSync(
    path.join(sketchDir, 'OpenBrewByWeightMachineMomentaryOnlyState.h'), 'utf8');
const relayHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineRelay.h'), 'utf8');
if (brewHeader.includes('machinePollIntention') ||
    brewHeader.includes('getScaleLinkSnapshot') ||
    brewHeader.includes('cupPresenceState(') ||
    brewHeader.includes('scaleAvailable(')) {
  throw new Error('Brew/guards must consume GuardInputs from the stopper — not poll machine/scale/cup');
}
if (scaleSenseHeader.includes('onFirstDropsDetected') ||
    scaleSenseHeader.includes('notifyCupPresenceTare') ||
    scaleSenseHeader.includes('holdCupPresenceTransitions')) {
  throw new Error('Scale sense must return events; the stopper applies brew/cup effects');
}
if (momentaryInputHeader.includes('session.active') ||
    momentaryConfigHeader.includes('session.active') ||
    momentaryControlHeader.includes('session.active') ||
    momentaryOnlyHeader.includes('session.active') ||
    momentaryOnlyHeader.includes('currentWeight') ||
    relayHeader.includes('session.automaticEnabled')) {
  throw new Error('Machine momentary/relay must not read session or live scale globals');
}
if (firmwareCore.includes('readRawActivatorOn()') ||
    firmwareCore.includes('pinMode(RELAY_GPIO') ||
    firmwareCore.includes('#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE') ||
    firmwareCore.includes('MACHINE_USES_MOMENTARY') ||
    firmwareCore.includes('stopPulseTenMs') ||
    firmwareCore.includes('maxSinglePressHundredMs') ||
    !firmware.includes('pinMode(RELAY_GPIO, OUTPUT)') ||
    !firmware.includes('machineBootActivatorHeldStably()') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachineActivatorSample.h'), 'utf8')
         .includes('bool rawActivatorOn') ||
    firmwareCore.includes('bool rawActivatorOn')) {
  throw new Error(
      'Machine GPIO, activator sample BSS, and boot debounce must live in machine headers — not openBrewByWeight.cpp');
}
if (!html.includes('<summary>Switch</summary>') ||
    !html.includes('id="stopPulseMs"') ||
    !html.includes('id="maxSinglePressMs"') ||
    !html.includes('id="momentaryStartEdge"') ||
    !html.includes('id="reedConfirmTimeoutS"') ||
    !html.includes('id="assumeIdleWhenScaleConnects"') ||
    !html.includes('id="shotReactTimeoutS"') ||
    !html.includes('class="reedOnly"') ||
    !html.includes('class="switchOnly"') ||
    !html.includes('class="cfgGroup momentaryOnly"') ||
    !html.includes('Auto-stop pulse') ||
    !html.includes('Longest tap') ||
    !html.includes('Start/stop on') ||
    !html.includes('Brew sensor wait') ||
    !html.includes('Pressed') ||
    !html.includes('Released') ||
    !html.includes('<strong>Pressed:</strong>') ||
    !html.includes('<strong>Released:</strong>') ||
    !html.includes('Starts or stops on press') ||
    !html.includes('a hold from idle becomes a rinse') ||
    !html.includes('Starts or stops on a short release') ||
    !html.includes('starts a rinse, never a shot') ||
    !html.includes('brew sensor has to confirm a start or stop') ||
    !html.includes('After that, it decides') ||
    !html.includes('Longer holds go to the machine') ||
    html.includes('<summary>Momentary</summary>') ||
    html.indexOf('<summary>Paddle</summary>') >
        html.indexOf('<summary>Switch</summary>') ||
    html.indexOf('<summary>Switch</summary>') >
        html.indexOf('<summary>Scale guard</summary>') ||
    html.indexOf('id="stopPulseMs"') >
        html.indexOf('<summary>Scale guard</summary>') ||
    !html.includes(
        'How long the controller presses the button to stop') ||
    !codeIncludes(ui, "stopPulseMs:$('stopPulseMs')?number(") ||
    !codeIncludes(ui, "if($('stopPulseMs'))$('stopPulseMs').value=") ||
    !codeIncludes(ui, 'Auto-stop pulse') ||
    !codeIncludes(ui, 'Longest tap') ||
    !codeIncludes(ui, 'Brew sensor wait') ||
    !codeIncludes(ui, 'momentaryStartEdge:') ||
    !codeIncludes(ui, 'reedConfirmTimeoutMs:') ||
    !codeIncludes(ui, 'assumeIdleWhenScaleConnects:') ||
    !codeIncludes(ui, 'shotReactTimeoutS:') ||
    !codeIncludes(ui, 'id="overrideIdleLink"') ||
    !codeIncludes(ui, 'id="overrideBrewingLink"') ||
    !codeIncludes(ui, 'id="machineStateValue"') ||
    !codeIncludes(ui, '/api/v1/control/state-override') ||
    !codeIncludes(ui, 'updateHomeAdminActions') ||
    !codeIncludes(ui, 'function updateHomeAdminActions(unlocked,remoteEnabled){const panel=__REMOTE_CONTROL__&&$(') ||
    !codeIncludes(ui, 'show=!!unlocked&&!!remoteEnabled') ||
    !codeIncludes(ui, 'syncAdminSessionUi(admin,remoteReady)') ||
    codeIncludes(ui, 'id="overrideIdleButton"') ||
    codeIncludes(ui, 'id="overrideBrewingButton"') ||
    codeIncludes(ui, "d.classList.contains('momentaryMachine')&&!d.classList.contains('reedMachine')") ||
    !html.includes('Assume idle when the scale connects') ||
    !html.includes('Marks the machine idle when a scale connects') ||
    !html.includes('Shot reaction timeout') ||
    !html.includes(
        'No coffee on the scale this long after Start') ||
    !html.includes('Mark as idle') ||
    !html.includes('Mark as running') ||
    !css.includes('html.reedMachine .switchOnly') ||
    !css.includes('html:not(.momentaryMachine) .switchOnly') ||
    !css.includes('#machineStateValue .switchOnly') ||
    !css.includes('#machineStateValue .switchOnly,#machineStateValue a{font-size:.78rem;font-weight:400}') ||
    !css.includes('.momentaryOnly') ||
    !css.includes('html:not(.reedMachine) .reedOnly') ||
    !network.includes('"stopPulseMs"') ||
    !network.includes('"momentaryStartEdge"') ||
    !network.includes('"reedConfirmTimeoutMs"') ||
    !network.includes('stopPulseMs must be an integer from 50 to 1000.') ||
    !network.includes('maxSinglePressMs must be an integer from 100 to 5000.') ||
    !network.includes('momentaryStartEdge must be press or release.') ||
    !network.includes(
        'reedConfirmTimeoutMs must be an integer from 200 to 5000.') ||
    !network.includes(
        'Shot reaction timeout must be 0 (compiled default) or from 3 to 30 s.') ||
    !network.includes(
        'shotReactTimeoutS must be 0 (compiled default) or an integer from 3 to 30.') ||
    !network.includes('jsonStopPulseMs') ||
    !network.includes('jsonMomentaryStartEdge') ||
    !network.includes('jsonReedConfirmTimeoutMs') ||
    !network.includes('jsonAssumeIdleWhenScaleConnects') ||
    !network.includes('jsonShotReactTimeoutS') ||
    !network.includes('stateOverrideHandler') ||
    !network.includes('/api/v1/control/state-override') ||
    !firmware.includes('machineApplyWorkflowConfig') ||
    !firmware.includes('dst.stopPulseTenMs = src.stopPulseTenMs') ||
    !momentaryControlHeader.includes(
        'dst.momentaryStartOnPress = src.momentaryStartOnPress') ||
    !momentaryControlHeader.includes(
        'dst.reedConfirmTimeoutHundredMs = src.reedConfirmTimeoutHundredMs') ||
    !momentaryControlHeader.includes(
        'dst.assumeIdleWhenScaleConnects = src.assumeIdleWhenScaleConnects') ||
    !momentaryControlHeader.includes(
        'dst.shotReactTimeoutS = src.shotReactTimeoutS') ||
    paddlePolicyHeader.includes('momentaryStartOnPress') ||
    paddlePolicyHeader.includes('reedConfirmTimeoutHundredMs') ||
    paddlePolicyHeader.includes('assumeIdleWhenScaleConnects') ||
    paddlePolicyHeader.includes('shotReactTimeoutS') ||
    machineTypesHeader.includes('parseMomentaryStartEdge') ||
    machineTypesHeader.includes('momentaryStartEdgeId') ||
    machineTypesHeader.includes('PaddleMode') ||
    machineTypesHeader.includes('parsePaddleMode') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightMachinePaddleConfig.h'), 'utf8')
         .includes('parsePaddleMode') ||
    !momentaryConfigHeader.includes('parseMomentaryStartEdge') ||
    !momentaryConfigHeader.includes('momentaryStartEdgeId') ||
    !domain.includes('uint8_t stopPulseTenMs') ||
    !domain.includes('bool momentaryStartOnPress') ||
    !domain.includes('setRuntimeStopPulseMs') ||
    !domain.includes('setRuntimeReedConfirmTimeoutMs') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8')
        .includes('stopPulseTenMs') ||
    fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrew.h'), 'utf8')
        .includes('stopPulseTenMs')) {
  throw new Error(
      'Momentary Switch timings must be wired in Settings, API, APPLY_CONFIG, and runtime config — not brew');
}
{
  const assert = require('assert'), vm = require('vm'), classes = new Set(['hidden']);
  const toggle = (name, on) => on ? classes.add(name) : classes.delete(name);
  const context = vm.createContext({__REMOTE_CONTROL__: true, $: () => ({classList: {toggle}}),
    document: {body: {classList: {toggle: () => {}}}}});
  vm.runInContext(blockAt(runtimeJs, 'function updateHomeAdminActions('), context);
  const visible = (admin, remote) => { vm.runInContext(`updateHomeAdminActions(${admin},${remote})`, context); return !classes.has('hidden'); };
  assert(!visible(false, false) && !visible(true, false) && !visible(false, true) && visible(true, true));
}
if (!html.includes('class="cfgGroup paddleOnly"><summary>Paddle</summary>') ||
    !html.includes('class="cfgGroup"><summary>Quick rinse</summary>') ||
    html.includes('class="cfgGroup paddleOnly"><summary>Quick rinse</summary>') ||
    !html.includes('id="rinseEnabled" type="checkbox"') ||
    html.indexOf('id="rinseEnabled"') >
        html.indexOf('id="rinseGestureS"') ||
    html.indexOf('<summary>Quick rinse</summary>') >
        html.indexOf('id="rinseEnabled"') ||
    !html.includes('id="rinseEnabled" type="checkbox"> Quick rinse') ||
    !html.includes('Flick the paddle ON and straight back OFF to rinse') ||
    !html.includes('Hold the switch from idle to rinse') ||
    !html.includes('Hold this long from idle to start a rinse') ||
    !html.includes('Water runs this long, then stops') ||
    !html.includes('a long press is left to the machine') ||
    !html.includes('id="rinseButton" class="btnGlyph" title="Start rinse"') ||
    html.includes('id="rinseButton" class="btnGlyph paddleOnly"') ||
    !codeIncludes(ui, 's.config.rinseEnabled===true') ||
    !codeIncludes(ui, 'rinseEnabled:$(\'rinseEnabled\').checked') ||
    !network.includes('"rinseEnabled"') ||
    !network.includes('rinseEnabled must be a boolean.') ||
    !firmwareCore.includes('candidate.rinseEnabled = command.config.rinseEnabled') ||
    !firmware.includes('uint32_t rinseBegin(') ||
    !firmware.includes('UserIntent::REQUEST_RINSE') ||
    !firmware.includes('machineBeginRinse') ||
    !html.includes('class="cfgGroup momentaryOnly"><summary>Switch</summary>') ||
    !html.includes('Paddle-off reminder<select id="paddleReturnReminder">') ||
    !html.includes('<option value="0">OFF</option><option value="15">15</option><option value="30" selected>30</option><option value="60">60</option><option value="120">120</option>') ||
    !html.includes('After an automatic stop, beeps at this interval until the paddle is back at OFF.') ||
    html.includes('id="paddleReturnReminderBeep"') ||
    html.includes('id="paddleReturnReminderIntervalS"') ||
    !codeIncludes(js, 'paddleReturnReminderBeep:reminder?!!+reminder.value:undefined') ||
    !codeIncludes(js, 'paddleReturnReminderIntervalMs:reminder?(+reminder.value||30)*1000:undefined') ||
    html.includes('cfgGroup paddleOnly momentaryOnly') ||
    html.includes('cfgGroup momentaryOnly paddleOnly') ||
    !css.includes('html.momentaryMachine .paddleOnly') ||
    !css.includes('html:not(.momentaryMachine) .momentaryOnly') ||
    !css.includes('html:not(.reedMachine) .reedOnly') ||
    !codeIncludes(ui, "classList.toggle('momentaryMachine',t!=='paddle')") ||
    !codeIncludes(ui, "classList.toggle('reedMachine',t==='momentary_reed')")) {
  throw new Error(
      'Paddle and Momentary Settings groups must be mutually exclusive by compiled machine type; Quick rinse is shared');
}
const rinseHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightRinse.h'), 'utf8');
const brewTypesHeader = fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBrewTypes.h'), 'utf8');
if (rinseHeader.includes('session.') ||
    rinseHeader.includes('runtimeConfig') ||
    brewHeader.includes('runtimeConfig.rinseGestureMs') ||
    brewTypesHeader.includes('DEFAULT_RINSE_GESTURE_MS') ||
    brewTypesHeader.includes('DEFAULT_RINSE_DURATION_MS') ||
    brewTypesHeader.includes('ENTER_RINSE =') ||
    firmware.includes('machineRinseGestureMs') ||
    domainCore.includes('snapshot.rinseGestureMs') ||
    !firmwareCore.includes('session.rinseStartedAtMs = rinseBegin') ||
    !firmwareCore.includes(
        'blockedHoldTimeoutMs = runtimeConfig.rinseGestureMs') ||
    !brewHeader.includes('inputs.blockedHoldTimeoutMs')) {
  throw new Error(
      'Rinse clock must not write session; brew must not read rinseGestureMs; the orchestrator copies the accept anchor');
}
if (!codeIncludes(ui, 'id="learnedOffsetG"') ||
    !codeIncludes(ui, 'id="dripSetG"') ||
    !codeIncludes(ui, 'id="dripSetButton"') ||
    html.indexOf('id="learnedOffsetG"') > html.indexOf('id="dripBars"') ||
    html.indexOf('id="dripBars"') > html.indexOf('id="dripSetG"') ||
    html.indexOf('id="dripSetG"') > html.indexOf('id="dripSetButton"') ||
    html.indexOf('id="dripSetButton"') > html.indexOf('id="alphaSet"') ||
    !codeIncludes(ui, 'Expected drip weight after the shot stops') ||
    codeIncludes(ui, 'Save new reset values first') ||
    !css.includes('.bbwLearning .btnGlyph{flex:0 1 11rem}') ||
    css.includes('#bbwAlgorithm{width:100%}') ||
    !network.includes('"dripG"') ||
    !network.includes('formatDripChartJson(drip, sizeof(drip), control)') ||
    !fs.readFileSync(path.join(sketchDir, 'OpenBrewByWeightBbwQuickStart.h'), 'utf8').includes('bbwRecentG')) {
  throw new Error('Drip compensation must show the drip, its recent shots and Set drip in order');
}
if (!html.includes('<summary>Cup detection</summary>') ||
    !html.includes('<summary>Tare</summary>') ||
    html.indexOf('<summary>Tare</summary>') >
        html.indexOf('<summary>Cup detection</summary>') ||
    ['minimumCupWeightG', 'cupRemovedWeightG', 'retareStabilitySamples',
     'retareStabilityToleranceG', 'retareStabilityMaxGapS',
     'retareStabilityMinDurationS'].some(id =>
      html.indexOf(`id="${id}"`) < html.indexOf('<summary>Cup detection</summary>') ||
      html.indexOf(`id="${id}"`) > html.indexOf('<summary>Cup protection</summary>')) ||
    !html.includes('<summary>Scale</summary>') ||
    html.includes('<summary>Scale & retare</summary>') ||
    html.indexOf('<summary>Scale</summary>') >
        html.indexOf('<summary>Tare</summary>') ||
    html.indexOf('<summary>Tare</summary>') >
        html.indexOf('<summary>Alerts</summary>') ||
    html.indexOf('id="autoTare"') < html.indexOf('<summary>Tare</summary>') ||
    html.indexOf('id="postTareBaselineGraceS"') <
        html.indexOf('id="autoTare"') ||
    html.indexOf('id="postTareBaselineGraceS"') >
        html.indexOf('id="autoRetare"') ||
    html.indexOf('id="autoRetare"') > html.indexOf('<summary>Cup detection</summary>') ||
    html.indexOf('id="retareWindowS"') > html.indexOf('<summary>Cup detection</summary>') ||
    html.indexOf('id="scaleTimerStopExtraDelayMs"') <
        html.indexOf('<summary>Scale</summary>') ||
    html.indexOf('id="dripDelayS"') <
        html.indexOf('id="scaleTimerStopExtraDelayMs"') ||
    html.indexOf('id="scalePreference"') <
        html.indexOf('id="dripDelayS"') ||
    html.indexOf('id="scalePreference"') <
        html.indexOf('<summary>Scale</summary>') ||
    html.indexOf('id="preferredScaleSelect"') <
        html.indexOf('<summary>Scale</summary>') ||
    html.indexOf('id="forgetPairedScale"') <
        html.indexOf('<summary>Scale</summary>') ||
    html.indexOf('<summary>Bookoo</summary>') <
        html.indexOf('<summary>Scale</summary>') ||
    html.indexOf('id="canTareStartTimer"') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('id="canTareStartTimer"') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('id="bookooMuteOnBuzzerOnly"') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('id="bookooMuteOnBuzzerOnly"') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('id="bookooConnectBeepLevel"') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('id="bookooConnectBeepLevel"') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('<strong>Needs tare at shot start.</strong>') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('<strong>Needs tare at shot start.</strong>') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('Applies when <strong>Buzzer only</strong> is selected.') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('Applies when <strong>Buzzer only</strong> is selected.') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('<strong>Scale first or Scale only</strong>') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('<strong>Scale first or Scale only</strong>') >
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('<summary>Acaia</summary>') <
        html.indexOf('<summary>Bookoo</summary>') ||
    html.indexOf('<summary>Felicita</summary>') <
        html.indexOf('<summary>Acaia</summary>') ||
    html.indexOf('<summary>Felicita</summary>') >
        html.indexOf('<summary>Alerts</summary>') ||
    !codeIncludes(ui, "d.parentElement.closest('details')")) {
  throw new Error('Machine settings must split Tare and Scales, with Bookoo/Acaia/Felicita subgroups');
}
if (!html.includes('id="postTareBaselineGraceS" type="number" min="0.5" max="10" step="0.1"') ||
    !codeIncludes(ui, "rangeCheck('postTareBaselineGraceS',0.5,10,'Post-tare grace',{unit:'s',}") ||
    !codeIncludes(ui, "postTareBaselineGraceMs:sToMs('postTareBaselineGraceS')") ||
    !codeIncludes(ui, "'postTareBaselineGrace'") ||
    !codeIncludes(ui, "$(k+'S').value=String(c[k+'Ms']/1000)") ||
    !codeIncludes(ui, "apply('tareOpt',!$('autoTare').checked)") ||
    // Tare switches go through the shared #workflowPanel field listener.
    !codeIncludes(viewJs.settings, 'noteEdit(autosave[sec],!typed?300:e.type===change&&e.isTrusted?0:1200);updateConfigGroups();') ||
    !network.includes('fields.field("config.postTareBaselineGraceMs",') ||
    !network.includes('Post-tare grace must be from 0.5 to 10 s.') ||
    !network.includes('candidate.postTareBaselineGraceMs') ||
    !firmware.includes('session.config.postTareBaselineGraceMs')) {
  throw new Error('Post-tare grace must be wired through Tare settings, the Settings stream, and firmware');
}
if (!codeIncludes(ui, "rangeCheck('dripDelayS',0,10,'Drip delay',{unit:'s',}") ||
    !codeIncludes(ui, "dripDelayMs:sToMs('dripDelayS')") ||
    !codeIncludes(ui, "$('dripDelayS').value=String((c.dripDelayMs??3000)/1000)") ||
    !codeIncludes(ui, 'The yield is recorded this long after the stop.') ||
    !network.includes('fields.field("config.dripDelayMs",') ||
    !network.includes('Drip delay must be from 0 to 10 s.') ||
    !network.includes('candidate.dripDelayMs')) {
  throw new Error('Drip delay must be wired through Settings, the Settings stream, and config validation');
}
if (!html.includes('<summary>AtomHeart Eclair</summary>') ||
    !html.includes('Eclair uses the regular tare and timer workflow') ||
    !html.includes('It has no extra volume or sound options') ||
    html.indexOf('<summary>AtomHeart Eclair</summary>') <
        html.indexOf('<summary>Felicita</summary>') ||
    html.indexOf('<summary>AtomHeart Eclair</summary>') >
        html.indexOf('<summary>Alerts</summary>') ||
    html.includes('id="eclair')) {
  throw new Error('Machine settings must include an informational AtomHeart Eclair subgroup without settings');
}
{
  const at = viewJs.home.indexOf('function renderReadyMsg(');
  const msg = {hidden: true, textContent: ''};
  const render = new Function('$', 'R', viewJs.home.slice(at, viewJs.home.indexOf('\n}', at) + 2) +
    ';return renderReadyMsg;')(() => msg, {reveal: (el, show) => { el.hidden = !show; }});
  const show = (mode, enabled, armed) => (render({state: 'IDLE', config: {brewByWeight: true, noScaleBbwMode: mode},
    scale: {available: false}, noScaleShotGuard: {enabled, armed}}), msg.hidden ? '' : msg.textContent);
  if (!/blocked once/.test(show('warn_once', true, true)) || /blocked once/.test(show('warn_once', true, false)) ||
      !/Connect a scale/.test(show('require_scale', true, true)) || /Connect a scale/.test(show('require_scale', true, false)) ||
      !/yourself/.test(show('off', false, false)))
    throw new Error('Home no-scale notice must follow the live guard: armed warns or requires, disarmed is manual');
}
{
  // Stats shot preset chart (plan AC1-AC3, AC5, AC8-AC9): the model comes from the
  // shot's stored snapshot, never the live preset; invalid snapshots keep the old card.
  const presetModel = runtimeJs.slice(runtimeJs.indexOf('function buildRuleChartModel('),
      runtimeJs.indexOf('function positionPresetScrub('));
  const shotModel = runtimeJs.slice(runtimeJs.indexOf('function shotRuleModel('),
      runtimeJs.indexOf('let shotPresetObserver'));
  const {shotRuleModel} = new Function('presetState',
      presetModel + shotModel + ';return {shotRuleModel};')({items: [{id: 1, name: 'Live', goalWeightG: 99}]});
  const row = {goalG: 36, fastBeforeS: 10, slowAfterS: 20, delayS: 5, wallS: 30, fastRampS: 1,
    slowRampS: 2, fastCurve: 'late', fastCurveHardness: 'hard', slowCurve: 'even',
    slowCurveHardness: 'soft',
    extractionGuardEnabled: true, slowExtractionGuardEnabled: true, fastMarginG: 2, slowMarginG: 2,
    firstDropS: 3, shotType: 'auto', verdict: 'on_recipe'};
  const live = shotRuleModel(row);
  if (live.mode !== 'active' || live.goal !== 36 || live.t1 !== 10 || live.dFast !== 1 ||
      live.dSlow !== 2 || live.curveFast !== 8 || live.curveSlow !== 0 || live.prot !== 5 ||
      live.wall !== 30)
    throw new Error('A Stats shot must chart its own stored snapshot');
  const timer = shotRuleModel({...row, shotType: 'timer_only'});
  if (timer.mode !== 'timerOnly' || timer.fast || timer.slow)
    throw new Error('Timer-only shots must draw every cut segment off');
  if (shotRuleModel({...row, delayS: 12}).mode !== 'empty')
    throw new Error('An invalid snapshot (delay not before the window) must render without the preset chart');
  const cardSrc = viewJs.stats.slice(viewJs.stats.indexOf('function renderShotSpark('), viewJs.stats.indexOf('function fillShotCharts('));
  if (cardSrc.includes('aria-expanded') || cardSrc.includes('shotMore') ||
      !cardSrc.includes('R.bindScrub(') || !/"role",\s*"slider"/.test(runtimeJs) || !cardSrc.includes('dataset.zone') ||
      !cardSrc.includes('observePresetHost(preset)') || cardSrc.includes('hatch') ||
      !cardSrc.includes('h.append(preset, readout, legend, combo);') || !cardSrc.includes('fillShotCharts(combo, m);') ||
      !cardSrc.includes('if (preset._view) h.style.setProperty("--shot-plot-h", preset._view.B - preset._view.T + "px");') ||
      !cardSrc.includes('m.timeMax = Math.max(rule.wall, m.dur);') || !cardSrc.includes('maxT: m.timeMax,') ||
      !js.includes('rate.className = "shotRateCell";') || runtimeJs.includes('shotInfo') ||
      !js.includes('<span class="heroPill" data-kind="'))
    throw new Error('The Stats preset card must show its details, preset chart and combined weight/flow chart at once (same plot height and time axis), plus a keyboard cursor, a verdict zone and pill, and no no-cut hatch');
  if (!css.includes('.shotPresetChart:not([data-zone=on_recipe]) .rvWindow') || css.includes('.shotMore') ||
      css.includes('aria-expanded=false') ||
      !css.includes('.shotComboHost{padding-top:1rem}') ||
      !css.includes('}[data-kind=fast]{--z:var(--gFast)}') || !css.includes('#shotTable .heroPill{') ||
      !js.includes('fill="url(#${u + cls})"') ||
      !js.includes('["rvWindow", "ok", 0.42, 0.02]') || !js.includes('["rvFastFull", "gFast", 0.36, 0]') ||
      !js.includes('flow ? 0.42 : 0.36') ||
      !css.includes('margin:.55rem 0 0;z-index:1;background:var(--sf)}') ||
      !css.includes('--gCut:#d1101f') || !css.includes('--gCut:#9e1c22') || !css.includes('.rvCut{fill:none;stroke:var(--gCut)'))
    throw new Error('Stats preset styles must color only the verdict zone and use the red cut-line token in both themes');
}
// Settings lists the Hero card above its colour modes, and Shot card colours
// stay read-only unless the resolved hero is Colorful: Technical tints its
// own graph zones, so the choice would do nothing there. The availability
// re-syncs when the card choice changes and when a status apply refreshes
// the machine default behind "Machine default".
{
  const settingsHtml = rawPartialHtml.settings,
    cardAt = settingsHtml.indexOf('id="appHeroCard"'),
    tintAt = settingsHtml.indexOf('id="appHeroTint"');
  if (cardAt < 0 || tintAt < 0 || cardAt > tintAt)
    throw new Error('Settings must list the Hero card above Shot card colours');
  if (!codeIncludes(rawViewJs.settings,
        '$("appHeroTint").disabled = R.heroCardMode() === "technical"') ||
      !codeIncludes(rawViewJs.settings,
        'setHeroCardOverride($("appHeroCard").value); syncHeroTintAvailability();') ||
      !codeIncludes(rawViewJs.settings,
        'applyStatus(s); syncHeroTintAvailability();'))
    throw new Error('Shot card colours must follow the resolved hero mode and go read-only on Technical');
}
