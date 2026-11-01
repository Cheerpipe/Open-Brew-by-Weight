// Exercise the production Stats sender with paced rows and observable store
// captures. Session/page transitions must neither recopy immutable data nor
// combine an old header with a new store generation. The record index for the
// browser's offline copy shares the store copy, so it runs here too.
{
  const assert = require('assert').strict, {spawnSync} = require('child_process');
  const read = (file) => fs.readFileSync(path.join(sketchDir, file), 'utf8');
  const source = read('network/OpenBrewByWeightHomeStream.inc') + read('network/OpenBrewByWeightRecordStreams.inc');
  const headerSource = read('network/OpenBrewByWeightStatsHeader.inc');
  const networkSource = read('OpenBrewByWeightNetwork.cpp');
  const header = read('OpenBrewByWeightNetwork.h');
  const body = (text, signature) => {
    const start = text.indexOf(signature);
    assert(start >= 0, signature);
    return text.slice(start, text.indexOf('\n}', start) + 2);
  };
  const sessionStart = header.indexOf('  struct UiStreamSession {');
  const session = header.slice(sessionStart, header.indexOf('\n  };', sessionStart) + 5);
  const uiStream = read('network/OpenBrewByWeightUiStream.inc');
  const heldRows = body(uiStream, 'uint32_t uiStreamRowHash(') + '\n' + body(uiStream, 'bool uiStreamHeld(');
  const deltaStart = header.indexOf('  struct RecordIndexBaseline {');
  const deltaTypes = header.slice(deltaStart, header.indexOf('  RecordIndexWork *recordIndexWork_', deltaStart));
  const native = `
#include <algorithm>
#include <cassert>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightHistoryTypes.h"
#include "OpenBrewByWeightShotCurveTypes.h"
using namespace openbrewbyweight;
constexpr int ESP_OK=0,HTTPD_WS_TYPE_TEXT=1;
constexpr size_t kStatsStreamFrameBytes=20480,COMPANION_RECORD_ORIGIN=72;
constexpr uint32_t kStatsStreamPaceMs=100;
const char *uiStreamSendKind="";
bool uiStreamSendCleanAbort=false;
uint32_t now=1,storeEpoch=1,frozen=0,shots=3; unsigned captures=0,hashes=0,flashReads=0; int sendResult=ESP_OK;
// With onFlash, shot 1's curve is past the PSRAM cache: read from flash, busy during a shot.
bool onFlash=false,flashBusy=false;
uint32_t millis(){return now;}
struct TaskLockGuard {explicit TaskLockGuard(int){}};
struct httpd_ws_frame_t{int type;uint8_t *payload;size_t len;};
std::vector<std::string> frames;
int httpd_ws_send_frame_async(int,int,httpd_ws_frame_t *frame){
  if(sendResult==ESP_OK)frames.emplace_back(reinterpret_cast<char *>(frame->payload),frame->len);
  return sendResult;
}
${body(networkSource, 'struct StatsCurveHashCache {')};
${body(headerSource, 'struct StatsHeaderWork {')};
${body(networkSource, 'struct StatsStreamCache : StatsHeaderWork {')};
${body(header, 'struct UiStreamFingerprints {')};
using HeldRow = UiStreamFingerprints::HeldRow;
constexpr size_t kHeldRows = UiStreamFingerprints::kHeldRows;
${heldRows}
struct NetworkWorkBuf {
  static constexpr size_t kStatusJson=40960,kJsonItem=16000;
  char statusJson[kStatusJson]{},jsonItem[kJsonItem]{};
  StatsStreamCache statsCache;
  UiStreamFingerprints uiFingerprints[2]{};
  HistoryPage historyPage{};
} work;
auto *g_work=&work;
${body(networkSource, 'bool __attribute__((format(printf, 2, 3)))\nstatusJsonAppend(')}
${body(headerSource, 'bool jsonScratchAppend(')}
bool escapeJsonString(const char *in,char *out,size_t cap){snprintf(out,cap,"%s",in);return true;}
// The aggregate names the store generation it was built from.
bool buildIntegrationStats(const ShotStatsView &,char *out,size_t cap,size_t window,bool durations){
  assert(!durations);snprintf(out,cap,"{\\"shotCount\\":%u,\\"window\\":%zu}",storeEpoch,window);return true;
}
uint32_t uiStreamCurveHash(const ShotCurveRecord &curve,uint16_t count){
  ++hashes;return count ? curve.lastWeightCg : 0;
}
constexpr char kStatsRowColumns[]="id,weight,padding";
size_t formatShotStatsRow(NetworkWorkBuf &buffer,const ShotLogRecord &record,
                         const ShotCurveRecord *curve){
  assert(curve && curve->lastWeightCg==record.actualWeightCg);
  return snprintf(buffer.jsonItem,sizeof(buffer.jsonItem),
      "{\\"id\\":%u,\\"weight\\":%u,\\"padding\\":\\"%015000d\\"}",record.id,record.actualWeightCg,0);
}
// Good shots 3..1 newest first; with shots=4 a newer bad (short) shot 4.
void capture(ShotStatsRecordsSnapshot &out){
  ++captures;
  out.epoch=storeEpoch;out.count=shots;
  const uint32_t content=frozen?frozen:storeEpoch;
  for(size_t i=0;i<out.count;++i){
    out.records[i]={};out.records[i].id=shots-i;out.records[i].durationDs=shots-i>3?110:300;
    out.records[i].actualWeightCg=content*1000+3-out.records[i].id;
    out.records[i].sideFlags=shotLogPackRating(0,i==0?1:i==1?5:0);
    shotLogSetPresetId(out.records[i],i==1?2:1);
  }
}
// 130 activations, newest first, so the index spans two history pages;
// each one more moves the epoch from 77.
size_t activations=130;
void historyPage(HistoryPage &page,size_t offset,size_t limit,ShotLogSortDir dir,uint8_t types){
  assert(dir==ShotLogSortDir::Desc&&!types&&limit==HISTORY_PAGE_MAX);
  page.total=activations;page.start=offset;
  page.count=offset<activations?std::min(limit,activations-offset):0;
  page.hasMore=offset+page.count<activations;
  for(size_t i=0;i<page.count;++i){page.records[i]={};page.records[i].id=activations-offset-i;}
}
ShotCurveRead curveById(uint32_t id,ShotCurveRecord &curve,bool readFlash){
  resetShotCurveRecord(curve);
  if(id==0||id>shots) return ShotCurveRead::NONE;
  if(id==1&&onFlash&&!readFlash) return ShotCurveRead::STORED;
  if(id==1&&onFlash&&(++flashReads,flashBusy)) return ShotCurveRead::BUSY;
  appendShotCurvePoint(curve,(frozen?frozen:storeEpoch)*1000+3-id,0,false);
  return ShotCurveRead::FOUND;
}
// The delta property's store, newest first, and the frame fields a
// companion reads.
struct DeltaRow{uint32_t id;uint8_t rating;bool bad;};
std::vector<DeltaRow> deltaStore;
ShotLogRecord deltaRecord(const DeltaRow &row){
  ShotLogRecord r={};r.id=row.id;r.durationDs=row.bad?110:300;r.actualWeightCg=3600;
  r.sideFlags=shotLogPackRating(0,row.rating);return r;
}
void captureDelta(ShotStatsRecordsSnapshot &out){
  out.epoch=storeEpoch;out.count=deltaStore.size();
  for(size_t i=0;i<out.count;++i)out.records[i]=deltaRecord(deltaStore[i]);
}
bool frameHas(const std::string &f,const char *key){
  return f.find(std::string("\\"")+key+"\\":")!=std::string::npos;
}
uint32_t frameNumber(const std::string &f,const char *key){
  const size_t at=f.find(std::string("\\"")+key+"\\":");
  assert(at!=std::string::npos);
  return std::stoul(f.substr(at+strlen(key)+3));
}
std::vector<uint32_t> frameList(const std::string &f,const char *key){
  std::vector<uint32_t> out;const size_t at=f.find(std::string("\\"")+key+"\\":[");
  if(at==std::string::npos)return out;
  for(const char *p=f.c_str()+at+strlen(key)+4;*p!=']';){
    char *end;out.push_back(strtoul(p,&end,10));p=*end==','?end+1:end;
  }
  return out;
}
struct OpenBrewByWeightNetwork {
  static constexpr size_t WEB_UI_CLIENT_ID_CAPACITY=25;
${session}
  NetworkWorkBuf *workBuf_=&work;
  UiStreamSession uiStreams_[2];
${deltaTypes}
  // Counts the delta work made: a session without a key never makes it.
  RecordIndexWork *recordIndexWork_=nullptr;unsigned deltaAllocations=0;bool deltaMemory=true;
  RecordIndexWork *recordIndexWork(){
    if(!recordIndexWork_&&deltaMemory){++deltaAllocations;recordIndexWork_=new RecordIndexWork{};}
    return recordIndexWork_;
  }
  int dataMux_=0,server_=1;bool webUiOverrideActive_=false;uint32_t webUiOverrideUntilMs_=0;
  char activeWebUiClientId_[WEB_UI_CLIENT_ID_CAPACITY]="owner";
  std::atomic<bool> uiStreamUrgent_{false};
  struct {
    uint32_t (*shotLogEpoch)()=+[](){return storeEpoch;};
    void (*copyShotStatsRecords)(ShotStatsRecordsSnapshot &)=capture;
    ShotCurveRead (*copyShotCurveById)(uint32_t,ShotCurveRecord &,bool)=curveById;
    uint32_t (*historyEpoch)()=+[](){return uint32_t(activations-53);};
    void (*copyHistoryPage)(HistoryPage &,size_t,size_t,ShotLogSortDir,uint8_t)=historyPage;
    void (*recordStoreCounters)(bool,uint32_t &,uint32_t &)=
        +[](bool,uint32_t &generation,uint32_t &next){generation=storeEpoch+40;next=shots+1;};
  } callbacks_;
  bool appendRecordPageUi(const ControlStatusSnapshot &,size_t *used,bool){
    return statusJsonAppend(used,"\\"ui\\":{}");
  }
  // The companion archive's fetch-only link session: its frames start after
  // the reserved prefix and are recorded without it.
  UiStreamSession *linkSession=nullptr;
  bool companionRecordsSession(const UiStreamSession &s) const {return &s==linkSession;}
  bool companionRecordsSend(size_t used,bool wrap,bool &deferArm){
    assert(wrap&&used>COMPANION_RECORD_ORIGIN);deferArm=false;
    frames.emplace_back(work.statusJson+COMPANION_RECORD_ORIGIN,used-COMPANION_RECORD_ORIGIN);
    return true;
  }
  bool sendUiStreamFrame(UiStreamSession &,size_t,bool &);
  bool sendStatsStream(UiStreamSession &,const ControlStatusSnapshot &);
  bool sendRecordsIndex(UiStreamSession &,const ControlStatusSnapshot &);
};
${body(headerSource, 'bool appendStatsDensity(')}
${body(headerSource, 'bool formatStatsHeader(')}
${body(source, 'bool OpenBrewByWeightNetwork::sendUiStreamFrame(')}
${body(source, 'bool OpenBrewByWeightNetwork::sendStatsStream(')}
${body(source, 'bool OpenBrewByWeightNetwork::sendRecordsIndex(')}
int main(){
  // The header covers the newest window of eligible shots: timed arrivals
  // oldest first, a density curve per scope scaled to its peak, and the
  // presets and scales of the whole log.
  {
    StatsStreamCache &cache=work.statsCache;ShotStatsRecordsSnapshot &recent=cache.snapshot;
    recent.count=60;
    for(size_t i=0;i<recent.count;++i){
      ShotLogRecord &r=recent.records[i];r={};r.id=100-i;r.durationDs=300;
      r.actualWeightCg=i==1?150:3600;r.arrivalDs=i==0?SHOT_LOG_METRIC_MISSING:280+i;
      r.verdict=i==0?3:i==5?1:0;r.fastBeforeDs=250;r.slowAfterDs=320;
      // i=1, a bad shot, alone holds preset 9 and the Acaia: no facet lists them.
      shotLogSetPresetId(r,i==3?7:i==1?9:4);
      snprintf(r.presetName,sizeof(r.presetName),"%s",i==3?"Decaf":i==1?"Spoiled":"House");
      snprintf(r.scaleName,sizeof(r.scaleName),"%s",i==1?"Acaia":i<4?"Lunar":"");
    }
    assert(formatStatsHeader(cache,10,4));
    std::string text=cache.header;
    assert(text.rfind("{\\"shotCount\\":",0)==0&&text.compare(text.size()-3,3,"]}}")==0);
    // Eligible: i=0 and 2..10 (i=1 has no yield); timed: 2..10, oldest first.
    assert(text.find("\\"cons\\":{\\"t\\":[290,289,288,287,286,285,284,283,282],\\"p\\":[4,4,4,4,4,4,4,7,4]}")!=std::string::npos);
    assert(text.find("\\"dens\\":[{\\"lo\\":19,\\"hi\\":38,\\"a\\":25.0,\\"b\\":32.0,\\"shared\\":true,\\"n\\":[1,8,0],\\"h\\":[")!=std::string::npos);
    assert(text.find("]},\\"facets\\":{\\"presets\\":[[4,\\"House\\"],[7,\\"Decaf\\"]],\\"scales\\":[[")!=std::string::npos);
    assert(text.find(",\\"Lunar\\"]]}}")!=std::string::npos&&text.find("Acaia")==std::string::npos);
    // The active preset's own curve: one Decaf shot; a preset without timed
    // shots gets null, and one holding every timed shot repeats the first.
    assert(text.find("]},{\\"lo\\":")!=std::string::npos);
    const size_t peak=text.find(",100,"),h=text.find("\\"h\\":[");
    assert(h!=std::string::npos&&peak!=std::string::npos);
    assert(formatStatsHeader(cache,10,9)&&std::string(cache.header).find("]},null],")!=std::string::npos);
    for(size_t i=0;i<recent.count;++i)shotLogSetPresetId(recent.records[i],4);
    assert(formatStatsHeader(cache,10,4)&&std::string(cache.header).find("]},true],")!=std::string::npos);
    assert(std::string(cache.header).find("\\"p\\":4}")!=std::string::npos);
    // A wider window takes more shots; no timed shot draws no chart.
    assert(formatStatsHeader(cache,50,4)&&std::string(cache.header).find("\\"t\\":[330,")!=std::string::npos);
    for(size_t i=0;i<recent.count;++i)recent.records[i].arrivalDs=0;
    assert(formatStatsHeader(cache,10,4)&&std::string(cache.header).find("\\"dens\\":[null,null],\\"cons\\":{\\"t\\":[],\\"p\\":0}")!=std::string::npos);
    cache=StatsStreamCache{};
  }
  OpenBrewByWeightNetwork network;auto &session=network.uiStreams_[0];
  strcpy(session.clientId,"owner");session.fd=1;session.statsOn=true;
  ControlStatusSnapshot control;control.bootId=9;
  auto send=[&](auto &target){now+=kStatsStreamPaceMs;return network.sendStatsStream(target,control);};
  assert(send(session)&&session.statsPaging&&session.statsSent==1);
  assert(captures==1&&hashes==3);
  assert(send(session)&&session.statsSent==2);
  assert(send(session)&&!session.statsPaging);
  assert(captures==1&&hashes==3&&frames.size()==3);
  assert(frames[0].find("\\"stats\\":{\\"shotCount\\":1,\\"window\\":10,\\"dens\\":[null,null]")!=std::string::npos);
  assert(frames[1].find("\\"rowBase\\":1")!=std::string::npos);
  // Columns are named once, in the page's first frame.
  assert(frames[0].find("\\"cols\\":\\"id,weight,padding\\"")!=std::string::npos&&
         frames[1].find("\\"cols\\"")==std::string::npos);
  assert(send(session)&&frames.size()==3);
  // A new window with the same order reuses the capture AND the cached
  // per-row curve hashes (curves are immutable once committed).
  session.statsFetch=true;session.statsFetchRequest=2;session.statsFetchOffset=1;
  session.statsFetchLimit=1;
  assert(send(session)&&captures==1&&hashes==3);
  assert(!session.statsFetch&&frames.back().find("\\"id\\":2")!=std::string::npos);
  // Home's hero fetch has no standing page: rows only, no header, no page state.
  {
    auto &home=network.uiStreams_[1];strcpy(home.clientId,"owner");home.fd=2;
    assert(send(home)&&frames.size()==4);
    home.statsFetch=true;home.statsFetchRequest=7;home.statsFetchOffset=1;home.statsFetchLimit=1;
    assert(send(home)&&!home.statsFetch&&!home.statsOn&&frames.size()==5);
    assert(frames.back().find("\\"request\\":7")!=std::string::npos&&
           frames.back().find("\\"stats\\":")==std::string::npos&&
           frames.back().find("\\"cols\\":")!=std::string::npos);
    home={};
  }
  // A debug export can borrow records: invalidation must refill the cache.
  work.statsCache.valid=false;work.statsCache.snapshot.records[0].actualWeightCg=999;
  session.statsResync=true;
  assert(send(session)&&captures==2&&session.statsSent==1&&hashes==6);
  const uint32_t oldSeq=session.statsSeq;
  ++storeEpoch;
  assert(send(session)&&captures==3&&session.statsSeq==oldSeq+1);
  assert(session.statsSent==1&&frames.back().find("\\"shotCount\\":2")!=std::string::npos);
  // An epoch advance during capture must also restart the in-flight page.
  work.statsCache.valid=false;
  network.callbacks_.copyShotStatsRecords=+[](ShotStatsRecordsSnapshot &out){++storeEpoch;capture(out);};
  assert(send(session)&&session.statsSeq==oldSeq+2&&session.statsSent==1);
  assert(session.statsPageEpoch==3&&frames.back().find("\\"epoch\\":3")!=std::string::npos);
  network.callbacks_.copyShotStatsRecords=capture;
  const size_t delivered=frames.size();const unsigned copied=captures,hashed=hashes;
  sendResult=-1;uiStreamSendCleanAbort=true;
  assert(send(session)&&session.statsSent==1&&frames.size()==delivered);
  sendResult=ESP_OK;
  assert(send(session)&&session.statsSent==2);
  assert(captures==copied&&hashes==hashed);
  // A new session owns its cursor; immutable cache contents may be shared.
  auto &fresh=network.uiStreams_[1];strcpy(fresh.clientId,"owner");fresh.fd=2;fresh.statsOn=true;
  assert(send(fresh)&&fresh.statsSent==1&&captures==copied);
  strcpy(fresh.clientId,"superseded");
  assert(!send(fresh));
  // The store returns newest-first; the sorter cannot reuse rating/asc input.
  // Sort/dir toggles re-sort the cached records in place: no store recapture.
  strcpy(fresh.clientId,"owner");fresh.statsPaging=false;
  auto page=[&](ShotLogSort sort,ShotLogSortDir dir,int a,int b,int c,bool fetch=false){
    const unsigned before=captures;
    fresh.statsResync=!fetch;fresh.statsFetch=fetch;
    if(fetch){fresh.statsFetchSort=sort;fresh.statsFetchDir=dir;
      fresh.statsFetchOffset=0;fresh.statsFetchLimit=100;fresh.statsFetchRequest=5;}
    else{fresh.statsSort=sort;fresh.statsDir=dir;}
    frames.clear();for(int i=0;i<3;++i)assert(send(fresh));
    assert(!fresh.statsPaging&&frames.size()==3&&captures==before);
    const int expected[]={a,b,c};
    for(size_t i=0;i<3;++i)
      assert(frames[i].find("\\\"id\\\":"+std::to_string(expected[i]))!=std::string::npos);
  };
  page(ShotLogSort::Date,ShotLogSortDir::Asc,1,2,3);
  page(ShotLogSort::Date,ShotLogSortDir::Desc,3,2,1);
  page(ShotLogSort::Rating,ShotLogSortDir::Desc,2,3,1);
  page(ShotLogSort::Date,ShotLogSortDir::Desc,3,2,1,true);
  page(ShotLogSort::Rating,ShotLogSortDir::Desc,2,3,1);
  page(ShotLogSort::Date,ShotLogSortDir::Desc,3,2,1);
  // A standing refresh names the rows the page already holds by id: with row
  // content unchanged, a store epoch move sends ids instead of rows.
  frozen=storeEpoch;++storeEpoch;frames.clear();
  assert(send(fresh)&&!fresh.statsPaging&&frames.size()==1);
  assert(frames[0].find("\\"rows\\":[3,2,1],\\"rowBase\\":0,\\"more\\":false")!=std::string::npos);
  // A resubscribe (or reconnect) still sends whole rows.
  fresh.statsResync=true;frames.clear();
  assert(send(fresh)&&frames[0].find("\\"id\\":3")!=std::string::npos);
  while(fresh.statsPaging)assert(send(fresh));
  // A window-only change refreshes the header and names the held rows by id.
  fresh.statsWindow=50;frames.clear();
  assert(send(fresh)&&!fresh.statsPaging&&frames.size()==1&&captures==captures);
  assert(frames[0].find("\\"window\\":50")!=std::string::npos&&
         frames[0].find("\\"rows\\":[3,2,1]")!=std::string::npos);
  assert(send(fresh)&&frames.size()==1);
  // The filter compacts the order: totals and pages count matches only.
  fresh.statsFilter.presetCount=1;fresh.statsFilter.presets[0]=1;fresh.statsResync=true;frames.clear();
  for(int i=0;i<2;++i)assert(send(fresh));
  assert(!fresh.statsPaging&&frames.size()==2);
  assert(frames[0].find("\\"total\\":2,")!=std::string::npos&&frames[0].find("\\"id\\":3")!=std::string::npos);
  assert(frames[1].find("\\"id\\":1")!=std::string::npos&&frames[1].find("\\"more\\":false")!=std::string::npos);
  // A fetch's own filter (the export's empty one) leaves the standing page alone.
  fresh.statsFetch=true;fresh.statsFetchRequest=6;fresh.statsFetchOffset=0;fresh.statsFetchLimit=100;
  fresh.statsFetchFilter={};frames.clear();
  for(int i=0;i<3;++i)assert(send(fresh));
  assert(frames.size()==3&&frames[0].find("\\"total\\":3,")!=std::string::npos);
  frames.clear();assert(send(fresh)&&frames.empty());
  // The record index answers one store per dispatch: the epoch alone when
  // the copy holds it, else [id, 24-bit revision] pairs newest first.
  {
    auto index=[&](){return network.sendRecordsIndex(fresh,control);};
    fresh.recordsShots=fresh.recordsHistory=true;fresh.recordsShotsEpoch=storeEpoch;
    fresh.recordsHistoryEpoch=0;frames.clear();
    assert(index()&&!fresh.recordsShots&&fresh.recordsHistory&&frames.size()==1);
    assert(frames[0]==std::string("{\\"v\\":1,\\"type\\":\\"records\\",\\"boot\\":9,\\"store\\":\\"shots\\",\\"epoch\\":")+
           std::to_string(storeEpoch)+"}");
    assert(index()&&!fresh.recordsHistory&&frames.size()==2);
    assert(frames[1].find("\\"store\\":\\"history\\",\\"epoch\\":77,\\"rows\\":[130,")!=std::string::npos);
    assert(frames[1].find(",1,")!=std::string::npos&&frames[1].find(",0,")==std::string::npos);
    size_t pairs=0;for(char c:frames[1])pairs+=c==',';
    assert(pairs==130*2-1+6);  // 5 header commas and the one before sum
    // A moved store lists every record; content (a rating) moves its revision.
    auto shotRows=[&](){
      fresh.recordsShots=true;fresh.recordsShotsEpoch=0;frames.clear();assert(index());
      const std::string f=frames[0];return f.substr(f.find("\\"rows\\":[")+8);
    };
    const std::string before=shotRows();
    assert(before.rfind("3,",0)==0&&before.find(",2,")!=std::string::npos&&before.find(",1,")!=std::string::npos);
    assert(shotRows()==before);
    frozen=0;++storeEpoch;
    const std::string after=shotRows();
    assert(after!=before&&after.rfind("3,",0)==0);
    for(const auto &r:{before,after}){
      size_t at=0;for(int i=0;i<3;++i){at=r.find(',',at)+1;assert(std::stoul(r.substr(at))<=0xFFFFFFu);at=r.find(',',at)+1;}
    }
    // A bad shot: left out of every page, the export and the offline index
    // unless the filter asks for it; the good shots keep their order.
    shots=4;++storeEpoch;
    const std::string goodRows=shotRows();
    assert(goodRows.rfind("3,",0)==0&&std::count(goodRows.begin(),goodRows.end(),',')==6);
    fresh.statsFilter={};fresh.statsSort=ShotLogSort::Date;fresh.statsDir=ShotLogSortDir::Desc;
    fresh.statsResync=true;frames.clear();
    for(int i=0;i<3;++i)assert(send(fresh));
    assert(!fresh.statsPaging&&frames.size()==3&&frames[0].find("\\"total\\":3,")!=std::string::npos);
    for(size_t i=0;i<3;++i)assert(frames[i].find("\\"id\\":"+std::to_string(3-i))!=std::string::npos);
    fresh.statsFilter.includeBad=true;fresh.statsResync=true;frames.clear();
    for(int i=0;i<4;++i)assert(send(fresh));
    assert(!fresh.statsPaging&&frames.size()==4&&frames[0].find("\\"total\\":4,")!=std::string::npos);
    for(size_t i=0;i<4;++i)assert(frames[i].find("\\"id\\":"+std::to_string(4-i))!=std::string::npos);
    page(ShotLogSort::Date,ShotLogSortDir::Desc,3,2,1,true);
    // The archive's link session: no standing page or header ever, every
    // shot in its index (the bad one too), fetches newest first by date.
    OpenBrewByWeightNetwork::UiStreamSession link;network.linkSession=&link;frames.clear();
    link.statsResync=true;assert(network.sendStatsStream(link,control)&&frames.empty());
    link.recordsShots=true;assert(network.sendRecordsIndex(link,control)&&frames.size()==1);
    const std::string all=frames[0].substr(frames[0].find("\\"rows\\":[")+8);
    assert(all.rfind("4,",0)==0&&std::count(all.begin(),all.end(),',')==8);
    // S19: the link's index names the store header counters.
    assert(frames[0].find(",\\"gen\\":"+std::to_string(storeEpoch+40)+",\\"next\\":5,")!=std::string::npos);
    link.statsFetch=true;link.statsFetchRequest=9;link.statsFetchOffset=0;link.statsFetchLimit=50;
    link.statsFetchFilter={};link.statsFetchFilter.includeBad=true;frames.clear();
    for(int i=0;i<10&&link.statsFetch;++i){now+=100;assert(network.sendStatsStream(link,control));}
    assert(frames.size()==4&&!link.statsFetch&&frames[0].find("\\"stats\\":")==std::string::npos);
    assert(frames[0].find("\\"request\\":9")!=std::string::npos&&frames[0].find("\\"good\\":3,")!=std::string::npos);
    // F2: a store copy reloaded by the index (same window shape) rebuilds the
    // link's order: the newest row follows an append and a delete.
    auto newest=[&](){
      link.statsFetch=true;link.statsFetchOffset=0;link.statsFetchLimit=1;++link.statsFetchRequest;
      frames.clear();for(int i=0;i<10&&link.statsFetch;++i){now+=100;assert(network.sendStatsStream(link,control));}
      return frames.at(0);
    };
    assert(newest().find("\\"id\\":4")!=std::string::npos);
    shots=5;++storeEpoch;link.recordsShots=true;assert(network.sendRecordsIndex(link,control));
    assert(newest().find("\\"id\\":5")!=std::string::npos);
    shots=4;++storeEpoch;link.recordsShots=true;assert(network.sendRecordsIndex(link,control));
    assert(newest().find("\\"id\\":4")!=std::string::npos);
    network.linkSession=nullptr;
    shots=3;
    // Sessions without a companion key never make the delta work.
    assert(network.deltaAllocations==0);
  }
  // Record index deltas: a companion's ui-stream session (good shots) and
  // its link session (every shot) follow a randomized store through their
  // own mirrors. Every answer must rebuild exactly the store's index and its
  // sum; a new session key, a deferred send or a Clear never leaves a stale
  // or partial mirror; and most changes leave as deltas.
  {
    const uint32_t epochBefore=storeEpoch;  // the later blocks' weights scale with it
    network.callbacks_.copyShotStatsRecords=captureDelta;work.statsCache.valid=false;++storeEpoch;
    OpenBrewByWeightNetwork::UiStreamSession link;network.linkSession=&link;
    OpenBrewByWeightNetwork::UiStreamSession *sessions[2]={&network.uiStreams_[0],&link};
    struct Mirror{std::vector<std::pair<uint32_t,uint32_t>> pairs;uint32_t epoch=0;bool known=false,fresh=false;};
    Mirror mirrors[2];
    uint32_t key=100,next=1,seed=12345;size_t deltas=0,wholes=0;
    for(auto *s:sessions)s->recordsKey=++key;
    auto rnd=[&](uint32_t n){seed=seed*1103515245u+12345u;return (seed>>8)%n;};
    auto ask=[&](int c){
      auto &s=*sessions[c];Mirror &m=mirrors[c];
      s.recordsShots=true;s.recordsShotsEpoch=m.known?m.epoch:0;s.recordsShotsDelta=m.known;
      frames.clear();
      assert(network.sendRecordsIndex(s,control));
      if(frames.empty())return;  // deferred: nothing reached the mirror
      assert(frames.size()==1&&!s.recordsShots);
      const std::string f=frames[0];
      if(frameHas(f,"rows")){
        const auto rows=frameList(f,"rows");m.pairs.clear();
        for(size_t i=0;i<rows.size();i+=2)m.pairs.emplace_back(rows[i],rows[i+1]);
        ++wholes;m.fresh=false;
      }else if(frameHas(f,"up")){
        assert(s.recordsShotsDelta&&!m.fresh&&frameNumber(f,"base")==m.epoch&&!frameHas(f,"rows"));
        const auto up=frameList(f,"up"),gone=frameList(f,"del");
        auto at=[&](uint32_t id){
          return std::find_if(m.pairs.begin(),m.pairs.end(),[&](const auto &p){return p.first==id;});
        };
        for(uint32_t id:gone){assert(at(id)!=m.pairs.end());m.pairs.erase(at(id));}
        for(size_t i=0;i<up.size();i+=2){
          if(at(up[i])!=m.pairs.end())at(up[i])->second=up[i+1];else m.pairs.emplace_back(up[i],up[i+1]);
        }
        std::sort(m.pairs.begin(),m.pairs.end(),[](const auto &a,const auto &b){return a.first>b.first;});
        ++deltas;
      }else{
        assert(frameNumber(f,"epoch")==m.epoch&&!frameHas(f,"sum"));
        return;
      }
      uint32_t crc=0xFFFFFFFFu;
      for(const auto &p:m.pairs){
        const uint32_t words[2]={p.first,p.second};
        crc=crc32Update(crc,reinterpret_cast<const uint8_t *>(words),sizeof(words));
      }
      assert(frameNumber(f,"sum")==~crc);
      m.epoch=frameNumber(f,"epoch");m.known=true;
      std::vector<std::pair<uint32_t,uint32_t>> want;
      for(const auto &row:deltaStore){
        if(c==0&&row.bad)continue;
        const ShotLogRecord r=deltaRecord(row);
        want.emplace_back(row.id,uiStreamRowHash(&r,sizeof(r))&0xFFFFFFu);
      }
      assert(m.pairs==want);
    };
    for(int step=0;step<600;++step){
      const uint32_t what=rnd(10);
      if(what<4){
        deltaStore.insert(deltaStore.begin(),DeltaRow{next++,uint8_t(rnd(6)),rnd(5)==0});
        if(deltaStore.size()>40)deltaStore.pop_back();  // the ring rotates its oldest out
      }else if(what<6&&!deltaStore.empty()){
        deltaStore[rnd(deltaStore.size())].rating=uint8_t(rnd(6));
      }else if(what<7&&!deltaStore.empty()){
        deltaStore.erase(deltaStore.begin()+rnd(deltaStore.size()));
      }else if(what==7&&rnd(6)==0){
        deltaStore.clear();next=1;  // Clear history: ids start again
      }
      if(what<8)++storeEpoch;
      const int c=rnd(2);
      if(what==8){sessions[c]->recordsKey=++key;mirrors[c].fresh=true;}
      if(what==9){sendResult=-1;uiStreamSendCleanAbort=true;}
      ask(c);
      if(what==9){sendResult=ESP_OK;uiStreamSendCleanAbort=false;ask(c);}
    }
    assert(deltas>wholes&&wholes>2);
    // At 1000 shots, a new one rotating the oldest out costs a small delta
    // instead of the whole index.
    deltaStore.clear();
    for(uint32_t id=101000;id>100000;--id)deltaStore.push_back(DeltaRow{id,uint8_t(id%6),false});
    mirrors[1]=Mirror{};++storeEpoch;ask(1);
    const size_t whole=frames.at(0).size();
    deltaStore.pop_back();deltaStore.insert(deltaStore.begin(),DeltaRow{101001,3,false});
    ++storeEpoch;ask(1);
    assert(whole>14000&&frameHas(frames.at(0),"up")&&frames[0].size()<200);
    // History keeps its own baseline, and each store's delta flag is its
    // own: the shots flag leaves history whole, then a new activation leaves
    // as one pair whose sum is the whole index's.
    auto &h=*sessions[0];
    auto history=[&](uint32_t held,bool delta){
      h.recordsHistory=h.recordsShotsDelta=true;h.recordsHistoryEpoch=held;h.recordsHistoryDelta=delta;
      frames.clear();assert(network.sendRecordsIndex(h,control)&&frames.size()==1&&!h.recordsHistory);
      return frames[0];
    };
    assert(frameHas(history(0,false),"rows"));
    activations=131;assert(frameHas(history(77,false),"rows"));
    activations=132;const std::string moved=history(78,true);
    assert(frameNumber(moved,"base")==78&&frameList(moved,"up").size()==2&&frameList(moved,"up")[0]==132&&
           frameList(moved,"del").empty()&&!frameHas(moved,"rows"));
    assert(frameNumber(moved,"sum")==frameNumber(history(0,false),"sum"));
    activations=130;
    // Without the memory, whole indexes keep the session working.
    network.deltaMemory=false;delete network.recordIndexWork_;network.recordIndexWork_=nullptr;
    deltaStore.pop_back();deltaStore.insert(deltaStore.begin(),DeltaRow{101002,1,false});
    ++storeEpoch;ask(1);
    assert(frameHas(frames.at(0),"rows"));
    network.deltaMemory=true;
    for(auto *s:sessions)s->recordsKey=0;
    network.linkSession=nullptr;deltaStore.clear();
    network.callbacks_.copyShotStatsRecords=capture;work.statsCache.valid=false;storeEpoch=epochBefore+1;
  }
  // A curve past the PSRAM cache: its row hash never reads flash; while a
  // shot runs the page holds at its row, sending nothing, and continues
  // once flash reads again.
  {
    onFlash=flashBusy=true;flashReads=0;fresh.statsFilter={};fresh.statsResync=true;
    ++storeEpoch;frames.clear();
    // Rows 3 and 2; framing 2 tries row 1, which is busy.
    assert(send(fresh)&&send(fresh)&&frames.size()==2&&flashReads==1);
    assert(send(fresh)&&fresh.statsPaging&&frames.size()==2&&flashReads==2);
    assert(send(fresh)&&fresh.statsPaging&&frames.size()==2&&flashReads==3);
    flashBusy=false;
    assert(send(fresh)&&!fresh.statsPaging&&frames.size()==3&&flashReads==4);
    assert(frames[2].find("\\"id\\":1,")!=std::string::npos);
    // Content unchanged: a standing refresh names the held rows, reading nothing.
    frozen=storeEpoch;++storeEpoch;frames.clear();
    assert(send(fresh)&&frames.size()==1&&flashReads==4);
    assert(frames[0].find("\\"rows\\":[3,2,1]")!=std::string::npos);
    onFlash=false;frozen=0;++storeEpoch;
  }
  // A full log of 1000 summaries (shots 4-1000 bad): the export window lists
  // the good shots only; with Show bad shots, a deep page and the ascending
  // order reach the oldest.
  {
    auto fetch=[&](uint32_t request,size_t offset,size_t limit,bool bad,ShotLogSortDir dir){
      fresh.statsFetch=true;fresh.statsFetchRequest=request;fresh.statsFetchOffset=offset;
      fresh.statsFetchLimit=limit;fresh.statsFetchFilter={};fresh.statsFetchFilter.includeBad=bad;
      fresh.statsFetchSort=ShotLogSort::Date;fresh.statsFetchDir=dir;frames.clear();
      while(fresh.statsFetch)assert(send(fresh));
    };
    shots=SHOT_LOG_CAPACITY;++storeEpoch;
    fetch(7,0,SHOT_LOG_CAPACITY,false,ShotLogSortDir::Desc);
    assert(frames.size()==3&&frames[0].find("\\"total\\":3,")!=std::string::npos);
    fetch(8,990,10,true,ShotLogSortDir::Desc);
    assert(frames.size()==10&&frames[0].find("\\"total\\":1000,")!=std::string::npos);
    assert(frames[0].find("\\"id\\":10,")!=std::string::npos&&frames[9].find("\\"id\\":1,")!=std::string::npos);
    fetch(9,0,2,true,ShotLogSortDir::Asc);
    assert(frames.size()==2&&frames[0].find("\\"id\\":1,")!=std::string::npos&&
           frames[1].find("\\"id\\":2,")!=std::string::npos);
    shots=3;++storeEpoch;
  }
  // Nothing is exported during a shot, drip included: a fetch (export window
  // or scroll page) sends nothing while the trace capture is open and
  // completes once it closes.
  {
    fresh.statsFetch=true;fresh.statsFetchRequest=11;fresh.statsFetchOffset=0;
    fresh.statsFetchLimit=3;fresh.statsFetchFilter={};fresh.statsFetchSort=ShotLogSort::Date;
    fresh.statsFetchDir=ShotLogSortDir::Desc;control.traceCapturing=true;frames.clear();
    for(int i=0;i<3;++i)assert(send(fresh));
    assert(frames.empty()&&fresh.statsFetch&&!fresh.statsPaging);
    control.traceCapturing=false;
    while(fresh.statsFetch)assert(send(fresh));
    assert(frames.size()==3&&frames[0].find("\\"request\\":11,")!=std::string::npos);
  }
}
`;
  const directory = path.resolve(sketchDir, '..', 'temp', 'ai_temp_stats_cache');
  fs.mkdirSync(directory, {recursive: true});
  const binary = path.join(directory, 'sender-' + process.pid);
  const compiled = spawnSync('c++', ['-std=c++17', '-DOPEN_BREW_BY_WEIGHT_HOST_TEST',
    '-I' + sketchDir, '-x', 'c++', '-', '-o', binary], {input: native, encoding: 'utf8'});
  assert.equal(compiled.status, 0, compiled.stderr);
  const run = spawnSync(binary, [], {encoding: 'utf8'});
  assert.equal(run.status, 0, String(run.signal) + ' ' + run.stderr);
  fs.unlinkSync(binary);
}
