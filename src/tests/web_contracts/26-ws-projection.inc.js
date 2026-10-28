// Execute the production serializers: omitted metrics cannot trigger a live
// object delta, but the full debug export must keep those measurements.
{
  const assert = require('assert').strict, {spawnSync} = require('child_process');
  const read = file => fs.readFileSync(path.join(sketchDir, file), 'utf8');
  const networkSource = read('OpenBrewByWeightNetwork.cpp');
  const body = (source, signature) => {
    const start = source.indexOf(signature);
    assert(start >= 0, signature);
    return source.slice(start, source.indexOf('\n}', start) + 2);
  };
  const native = `
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightTaskProfiler.h"
#include "OpenBrewByWeightScaleProfiler.h"
#include "OpenBrewByWeightShotCurveTypes.h"
using namespace openbrewbyweight;
struct NetworkWorkBuf {
  static constexpr size_t kStatusJson=40960,kJsonItem=28672;
  char statusJson[kStatusJson]{},jsonItem[kJsonItem]{},curveJson[SHOT_CURVE_JSON_CAPACITY]{};
  struct {ShotCurveRecord fetchCurve{};} statsCache;
} work;
auto *g_work=&work;
${body(networkSource, 'void sanitizeJsonEmbed(')}
${body(networkSource, 'bool jsonScratchAppend(')}
${body(networkSource, 'bool __attribute__((format(printf, 2, 3)))\nstatusJsonAppend(')}
${body(networkSource, 'bool formatTaskProfilerObject(')}
${body(networkSource, 'bool statusJsonAppendScaleProfiler(')}
struct OpenBrewByWeightNetwork {
  static size_t formatShotStatsRow(NetworkWorkBuf &,const ShotLogRecord &,const ShotCurveRecord *);
};
${body(read('network/OpenBrewByWeightStatsPage.inc'), 'size_t OpenBrewByWeightNetwork::formatShotStatsRow(')}
int main(){
  TaskProfilerSnapshot tasks;
  tasks.rowCount=1;strcpy(tasks.rows[0].name,"worker");tasks.rows[0].stackMinBytes=1024;
  tasks.loopPhases.rowCount=LOOP_PHASE_COUNT;tasks.loopPhases.peakGapAtMs=4242;
  for(auto &row:tasks.loopPhases.rows){row.name="test";row.sampleCount=5;row.recentGapExecutionUs=12;}
  tasks.scaleWorkerPhases.rowCount=SCALE_WORKER_PHASE_COUNT;
  for(auto &row:tasks.scaleWorkerPhases.rows){row.name="sec";row.sampleCount=9;}
  auto format=[&](bool full){size_t used=0;assert(formatTaskProfilerObject(work.statusJson,sizeof(work.statusJson),&used,tasks,full));return std::string(work.statusJson);};
  const auto live=format(false),full=format(true);
  puts(live.c_str());puts(full.c_str());
  tasks.remainingMs=900;tasks.sampleCount=20;tasks.averageTotalCpuPct=80;
  tasks.lastCaptureUs=100;tasks.maxCaptureUs=200;
  tasks.micraBackflushWaitLastUs=11;tasks.micraBackflushWaitMaxUs=22;
  tasks.micraBackflushHoldLastUs=13;tasks.micraBackflushHoldMaxUs=24;
  tasks.scaleEventsWaitLastUs=33;tasks.scaleEventsWaitMaxUs=44;
  tasks.shotStoreWaitLastUs=55;tasks.shotStoreWaitMaxUs=66;
  tasks.controlStatusWaitLastUs=77;tasks.controlStatusWaitMaxUs=88;
  for(auto &row:tasks.loopPhases.rows){row.maxCpuUs=7;row.maxAtMs=8;}
  for(auto &row:tasks.loopPhases.rows){row.averageExecutionUs=100;row.maxExecutionUs=1000;row.lastExecutionUs=500;}
  for(auto &row:tasks.scaleWorkerPhases.rows){row.averageExecutionUs=60;row.maxExecutionUs=700;row.lastExecutionUs=30;}
  assert(format(false)==live && format(true)!=full);
  const auto detailed=format(true);
  assert(detailed.find("\\\"maxCpuUs\\\":7,\\\"maxAtMs\\\":8")!=std::string::npos);
  assert(live.find("peakGapAtMs")==std::string::npos &&
         detailed.find("\\\"peakGapAtMs\\\":4242")!=std::string::npos);
  assert(detailed.find("\\\"lockWaits\\\":{\\\"micraBackflush\\\":{\\\"lastUs\\\":11,"
                      "\\\"maxUs\\\":22,\\\"holdLastUs\\\":13,\\\"holdMaxUs\\\":24},"
                      "\\\"scaleEvents\\\":{\\\"lastUs\\\":33,"
                      "\\\"maxUs\\\":44},\\\"shotStore\\\":{\\\"lastUs\\\":55,"
                      "\\\"maxUs\\\":66},\\\"controlStatus\\\":{\\\"lastUs\\\":77,"
                      "\\\"maxUs\\\":88}}")!=std::string::npos);
  ++tasks.loopPhases.rows[0].recentGapExecutionUs;
  assert(format(false)!=live);
  assert(detailed.find("\\\"name\\":\\"scaleWorker/sec\\"")!=std::string::npos);
  assert(live.find("\\\"name\\":\\"scaleWorker/sec\\"")!=std::string::npos);
  size_t used=0;assert(!formatTaskProfilerObject(work.statusJson,40,&used,tasks,false));
  ScaleProfilerStatus profile;profile.partitionAvailable=true;
  auto scale=[&](){size_t used=0;assert(statusJsonAppendScaleProfiler(&used,profile));return std::string("{")+work.statusJson+"}";};
  const auto empty=scale();puts(empty.c_str());
  ++profile.generation;++profile.sessionId;
  assert(scale()==empty);
  profile.downloading=true;assert(scale()!=empty);puts(scale().c_str());
  profile.downloading=false;profile.lastError=ScaleProfilerError::ALLOCATION;puts(scale().c_str());
  ShotLogRecord record{};record.id=1;record.bootId=9;record.goalWeightDg=400;record.actualWeightCg=4200;
  record.errorCg=200;record.avgFlowCgS=SHOT_LOG_METRIC_MISSING;
  record.firstDropDs=record.tareAtDs=SHOT_LOG_METRIC_MISSING;
  assert(OpenBrewByWeightNetwork::formatShotStatsRow(work,record,nullptr));
  const std::string row=work.jsonItem;puts(row.c_str());
  record.errorCg=500;
  assert(OpenBrewByWeightNetwork::formatShotStatsRow(work,record,nullptr));
  assert(row==work.jsonItem);
  // Codec cross-check: each line is a curve's appended points and its curve
  // bodies from three fragment starts, for the browser decoder below.
  static const uint16_t edgeAt[9]={0,254,509,765,893,1404,1404,1915,60000};
  static const int16_t edgeCg[9]={0,127,0,255,127,126,510,-32767,32767};
  uint32_t seed=0x2545F491u;
  auto rnd=[&]{return seed=seed*1664525u+1013904223u;};
  for(int c=0;c<40;++c){
    ShotCurveRecord curve=emptyShotCurveRecord();
    std::string line="{\\"pts\\":[";
    uint32_t at=0;int cg=0;
    const int wanted=c==0?9:static_cast<int>(rnd()%300);
    for(int i=0;i<wanted;++i){
      if(c==0){at=edgeAt[i];cg=edgeCg[i];}
      else{
        at+=rnd()%600;if(at>60000)break;
        cg+=static_cast<int>(rnd()%600)-300;
        if(rnd()%13==0)cg=static_cast<int>(rnd()%65535)-32767;
        cg=cg>32767?32767:cg<-32767?-32767:cg;
      }
      assert(appendShotCurvePoint(curve,static_cast<int16_t>(cg),at,rnd()%5==0,rnd()%7==0));
      line+=std::string(i?",[":"[")+std::to_string(at)+","+std::to_string(cg)+"]";
    }
    line+="],\\"frags\\":[";
    const uint16_t starts[3]={0,static_cast<uint16_t>(curve.count/3),curve.count};
    for(int f=0;f<3;++f){
      assert(formatShotCurveJsonBody(work.curveJson,sizeof(work.curveJson),curve,starts[f]));
      line+=std::string(f?",":"")+"{\\"start\\":"+std::to_string(starts[f])+",\\"curve\\":{"+work.curveJson+"}}";
    }
    puts((line+"]}").c_str());
  }
}
`;
  const directory = path.resolve(sketchDir, '..', 'temp', 'ai_temp_ws_projection_contract');
  fs.mkdirSync(directory, {recursive: true});
  const binary = path.join(directory, 'projection-' + process.pid);
  const compiled = spawnSync('c++', ['-std=c++17', '-DOPEN_BREW_BY_WEIGHT_HOST_TEST',
    '-I' + sketchDir, '-x', 'c++', '-', '-o', binary], {input: native, encoding: 'utf8'});
  assert.equal(compiled.status, 0, compiled.stderr);
  const run = spawnSync(binary, [], {encoding: 'utf8'});
  assert.equal(run.status, 0, run.stderr);
  fs.unlinkSync(binary);
  const wire = run.stdout.trim().split('\n');
  const [live, full, empty, busy, failed, rowValues, ...curves] = wire.map(JSON.parse);
  // A Stats row is its values in kStatsRowColumns order plus the curve object.
  const homeSource = read('network/OpenBrewByWeightStatsPage.inc');
  const columnsDecl = homeSource.slice(homeSource.indexOf('constexpr char kStatsRowColumns[] ='));
  const columns = [...columnsDecl.slice(0, columnsDecl.indexOf(';')).matchAll(/"([^"]*)"/g)]
      .map(m => m[1]).join('').split(',');
  assert(Array.isArray(rowValues) && rowValues.length === columns.length + 1 &&
      typeof rowValues.at(-1) === 'object', 'one value per column, then the curve');
  const row = {...Object.fromEntries(columns.map((k, i) => [k, rowValues[i]])), ...rowValues.at(-1)};
  // The browser decoder recovers every firmware-appended point, and a
  // fragment names exactly the point before it.
  const decode = new Function(blockAt(rawRuntimeJs, 'function decodeCurveDeltas(') +
      ';return decodeCurveDeltas;')();
  assert.equal(curves.length, 40);
  for (const {pts, frags} of curves) {
    for (const {start, curve} of frags) {
      assert.deepEqual(curve.wRef, start > 0 && start < pts.length ? pts[start - 1] : undefined);
      for (const key of ['wBreakBefore', 'wHeld'])
        assert(start > 0 ? curve[key]?.length !== 0 : Array.isArray(curve[key]),
            'a whole curve lists both index arrays; a fragment omits an empty one');
      const points = decode(curve.wD, curve.wRef);
      assert.deepEqual(points.wAtMs.map((t, i) => [t, points.wCg[i]]), pts.slice(start));
    }
  }
  for (const key of ['stackUnit','remainingMs','sampleCount','averageTotalCpuPct','lastCaptureUs','maxCaptureUs']) {
    assert(!(key in live), key);assert(key in full, 'debug export retains '+key);
  }
  for (const key of ['averageExecutionUs','maxExecutionUs','lastExecutionUs']) {
    assert(!(key in live.rows[1]), key);assert(key in full.rows[1]);
  }
  assert.equal(live.rows[1].sampleCount, 5);
  assert.equal(live.rows[1].recentGapExecutionUs, 12);
  assert.equal(live.rows[0].stackMinWords, 1024);
  // One task row plus every loop phase and scale-worker section row.
  assert.equal(live.rows.length, 1 + 28 + 5);
  const scaleRow = live.rows.find((r) => r.name === 'scaleWorker/sec');
  assert.equal(scaleRow.sampleCount, 9);
  assert(!(('averageExecutionUs') in scaleRow));
  assert(('averageExecutionUs') in full.rows.find((r) => r.name === 'scaleWorker/sec'));
  assert(Buffer.byteLength(JSON.stringify(full))-Buffer.byteLength(JSON.stringify(live)) >= 1000);
  for (const key of ['generation','sessionId','durationLimitMs','recordBytes','complete','downloading'])
    assert(!(key in empty.scaleProfile), key);
  assert.equal(empty.scaleProfile.canStart, true);
  assert.equal(busy.scaleProfile.canStart, false);
  assert.equal(failed.scaleProfile.lastError, 'allocation');
  assert.equal(row.bootId, 9);assert.equal(row.actualG, 42);assert.equal(row.goalG, 40);
  assert(!('errorG' in row) && !('errorPct' in row));
  assert(row.wD === '' && Array.isArray(row.wBreakBefore) && !('wCg' in row) && !('wAtMs' in row));
  fs.writeFileSync(path.join(directory, 'sizes.json'), JSON.stringify({
    fixture: 'one task and nineteen loop phases; one synthetic 42g shot',
    taskLiveBytes: Buffer.byteLength(wire[0]),
    taskFullBytes: Buffer.byteLength(wire[1]),
    shotRowBytes: Buffer.byteLength(wire[5])}, null, 2)+'\n');
}

// Actual presentation consumers must distinguish unavailable measurements and
// the firmware's lowercase stop reason. Zero remains a legitimate NVS value.
{
  const assert = require('assert').strict, vm = require('vm');
  const source = fs.readFileSync(path.join(sketchDir, 'web/js/runtime.js'), 'utf8');
  const section = (a, b) => source.slice(source.indexOf(a), source.indexOf(b, source.indexOf(a)));
  const nodes = new Map(), $ = id => {
    if (!nodes.has(id)) nodes.set(id, {textContent: ''});return nodes.get(id);
  };
  const action = {inert: true};
  const context = {$, __WEBUI_TEXT__: key => key, document: {querySelectorAll: () => [action]},
    R: {syncAdminSessionUi(){}, message(){}, uiState: {diagnosticPublicView: false}, diagnosticVisible: false,
      adminUnlockedFor: (s) => !!s.adminUnlocked},
    updH(){},ntpStateLabel(){},formatDiagnosticTimezoneOffset(){},renderDiagClock(){}};
  vm.createContext(context);
  vm.runInContext(blockAt(fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8'),
      'function applyDiagnosticStatus('), context);
  const status = {adminUnlocked:true,health:{},safety:{},maintenance:{},network:{},time:{},scale:{},
    config:{},nvs:{statsValid:false,usedEntries:0,availableEntries:20,freeEntries:0,totalEntries:20,namespaces:0}};
  context.applyDiagnosticStatus(status);
  assert.equal(action.inert, false, 'unlocked Admin keeps the Diagnostic actions usable');
  context.applyDiagnosticStatus({...status, adminUnlocked: false});
  assert.equal(action.inert, true, 'locked Admin with a hidden page disables refused actions');
  for (const id of ['hNvsEntries','hNvsTotal','hNvsNamespaces']) assert.equal($(id).textContent, '—');
  status.nvs.statsValid=true;context.applyDiagnosticStatus(status);
  assert.equal($('hNvsNamespaces').textContent,'0');assert($('hNvsEntries').textContent.startsWith('0'));
  const cup={__WEBUI_TEXT__:key=>key,$:()=>null,sub:key=>key};
  vm.createContext(cup);
  vm.runInContext(blockAt(fs.readFileSync(path.join(sketchDir, 'web/js/diagnostic.js'), 'utf8'),
      'function formatCupProtection('),cup);
  const idle={enabled:true,requireCupToStart:false,stopIfRemoved:false,present:false,scaleUsable:false,inShot:false};
  assert.equal(cup.formatCupProtection({...idle,aborted:true}),'runtime.shot_aborted');
  assert.equal(cup.formatCupProtection({...idle,aborted:false}),'runtime.brew_allowed');
}
