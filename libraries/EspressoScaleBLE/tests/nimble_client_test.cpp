#include "nimble_client_platform.h"
#include "EspressoScaleBLE.h"
#include "nimble/NimbleAdvertisement.h"
#include "nimble/NimbleResilience.h"
#include <cstdio>
#include <cassert>
#include <cstdarg>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <thread>

static int unlockedSnprintf(char *out, size_t capacity, const char *format, ...) {
  assert(testCriticalDepth == 0);
  va_list args;
  va_start(args, format);
  const int result = vsnprintf(out, capacity, format, args);
  va_end(args);
  return result;
}

#define snprintf unlockedSnprintf
#include "../src/EspressoScaleBLENimble.cpp"
#undef snprintf
static std::vector<std::pair<uint8_t,std::string>> capturedScaleLogs;
static std::vector<bool> capturedScaleLogTransport;
extern "C" void openBrewByWeightScaleLog(uint8_t severity, bool transport, const char *message) {
  capturedScaleLogs.emplace_back(severity,message==nullptr ? "" : message);
  capturedScaleLogTransport.push_back(transport);
}

static unsigned checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)

namespace {
struct NimbleScaleClientTest {
static void ready(NimbleScaleClient &c) {
  testOnWait={}; testOnDelay={}; testOnSubmit={}; testSubmitStatus=0;
  testOnConnectCancel={}; testOnTerminate={}; testConnectCancelStatus=0;
  testTerminateStatus=0; testRssiStatus=0; testRadioProcedures=0; testTerminations=0;
  testWrites=0; testWakeCount=0;
  capturedScaleLogs.clear(); capturedScaleLogTransport.clear();
  testRuntimeReady=true; testSyncGeneration=1;
  c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=1;
  c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
  c.connectionHandle_=1; c.readHandle_=10; c.writeHandle_=12;
  c.writeProperties_=BLE_GATT_CHR_PROP_WRITE;
  c.protocol_=&kScaleProtocolGenericFf11;
  c.finishReady();
}
static void complete(int status=0) {
  ble_gatt_error e={status,12};
  testWriteCallback(1,&e,nullptr,testWriteArg);
}
static void notify(NimbleScaleClient &c,uint16_t length) {
  os_mbuf b={}; b.length=length; b.data[0]=3;
  b.data[1]=0x0b;
  b.data[19]=0x08;
  c.onNotification(1,10,&b,c.linkOperationId_);
}
static void advertise(NimbleScaleClient &c, uint8_t eventType,
                      const char *name = "BOOKOO Mini") {
  testAdvertisementParseStatus = 0;
  testAdvertisementFields = {
      reinterpret_cast<uint8_t *>(const_cast<char *>(name)),
      static_cast<uint8_t>(strlen(name)), 1, 0, nullptr};
  ble_gap_disc_desc advertisement = {
      {0, {1, 2, 3, 4, 5, 6}}, eventType, nullptr, 0};
  c.onAdvertisement(advertisement, c.scanOperationId_);
}
static void run() {
  {
    NimbleScaleClient c(false);
    static NimbleScaleClient *observed = nullptr;
    static bool inhibited = false;
    observed = &c;
    EspressoScaleBLE facade(false);
    facade.setAcquisitionObserver([](bool held, uint32_t) {
      CHECK(testCriticalDepth == 0);
      if (held && !inhibited) CHECK(!observed->candidatePending_);
      inhibited = held;
    });
    testRuntimeReady = true;
    CHECK(c.beginConfiguredScan(false));
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_NONCONN_IND);
    CHECK(!inhibited);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(inhibited && c.candidatePending_);
    c.candidatePending_ = c.candidateQueued_ = false;
    testDiscoveryActive = false;
    c.service();
    testNowMs += 10000;
    c.service();
    CHECK(inhibited); // No scanning opportunity: absence cannot release it.
    testDiscoveryActive = true;
    c.service();
    testNowMs += 4999;
    c.service();
    CHECK(inhibited);
    ++testNowMs;
    c.service();
    CHECK(!inhibited);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(inhibited);
    c.enterState(NimbleScaleClient::State::Ready);
    CHECK(!inhibited);
    facade.setAcquisitionObserver(nullptr);
    observed = nullptr;
  }
  {
    EspressoScaleBLE facade(false);
    auto &c = clientFromStorage(g_clientStorage);
    for (unsigned operation = 0; operation < 3; ++operation) {
      ready(c);
      c.writeProperties_ = BLE_GATT_CHR_PROP_WRITE_NO_RSP;
      CHECK(facade.stopTimer() == ScaleCommandResult::Ok);
      bool shutdown = false;
      testOnDelay = [&] { shutdown = true; };
      const ScaleCommandAdmission admission{
          [](void *context, uint32_t &) { return !*static_cast<bool *>(context); }, &shutdown};
      const auto result = operation == 0 ? facade.stopTimer(&admission) :
          operation == 1 ? facade.beepWithoutStateChange(&admission) :
                           facade.setBeepLevel(1, &admission);
      CHECK(shutdown && result == ScaleCommandResult::Deferred && testWrites == 1);
      testOnDelay = {};
    }
  }
  {
    NimbleScaleClient c(false); ready(c);
    std::atomic<bool> start{false};
    std::thread host([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      ble_gap_event gone{};
      gone.type = BLE_GAP_EVENT_DISCONNECT;
      gone.disconnect.conn.conn_handle = 1;
      c.onGapEvent(&gone, c.linkOperationId_);
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 10000; ++i) {
      const auto snapshot = c.diagnostics();
      CHECK(snapshot.silenceSequence == 0 ||
            (snapshot.silenceStartedAtMs == testNowMs && snapshot.silenceTrigger != 0));
    }
    host.join();
    CHECK(c.diagnostics().silenceSequence == 1);
  }
  for (bool synchronous : {false,true}) {
    NimbleScaleClient c(false); ready(c);
    c.connectionHandle_=kInvalidHandle;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    ble_gap_event success={}; success.type=BLE_GAP_EVENT_CONNECT;
    success.connect.conn_handle=42;
    testConnectCancelStatus=BLE_HS_EALREADY;
    if (synchronous) testOnConnectCancel=[&] { c.onGapEvent(&success,operation); };
    c.disconnect();
    CHECK(c.cleanupCount_==1);
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    if (!synchronous) c.onGapEvent(&success,operation);
    testOnConnectCancel={};
    c.service();
    CHECK(testTerminations==1);
    c.onGapEvent(&success,operation); // Duplicate cannot submit twice.
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS+1;
    c.service();
    CHECK(testTerminations==1);
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
    gone.disconnect.conn.conn_handle=42;
    c.onGapEvent(&gone,operation);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS;
    CHECK(c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    c.onGapEvent(&success,operation);
    c.service(); CHECK(testTerminations==1);
    CHECK(c.cleanupCount_==1);
  }
  for (int cancelStatus : {0,BLE_HS_EALREADY,BLE_HS_EBUSY}) {
    for (bool reset : {false,true}) {
      NimbleScaleClient c(false); ready(c);
      c.connectionHandle_=kInvalidHandle;
      c.enterState(NimbleScaleClient::State::Connecting);
      const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
      testConnectCancelStatus=cancelStatus;
      c.disconnect();
      const unsigned procedures=testRadioProcedures;
      testNowMs+=SCALE_DISCONNECT_SILENCE_MS*2;
      for (unsigned retry=0;retry<8;++retry) {
        c.service();
        CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
      }
      CHECK(testRadioProcedures==procedures); // Missing callback cannot retry storm.
      ble_gap_event event={}; event.type=BLE_GAP_EVENT_CONNECT;
      event.connect.conn_handle=42;
      c.onGapEvent(&event,operation+1); // An unrelated stale success owns nothing.
      c.service(); CHECK(testTerminations==0);
      if (reset) {
        ++testSyncGeneration;
        c.service();
        c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=testSyncGeneration;
        c.enterState(NimbleScaleClient::State::Connecting);
        const uint32_t replacement=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
        c.onGapEvent(&event,replacement);
        c.onGapEvent(&event,operation); // Same handle, previous runtime epoch.
        CHECK(c.connectionHandle_==42);
        CHECK(!c.linkCleanupPending());
        CHECK(testTerminations==0);
        // Leave no in-flight discovery for this direct callback countercase.
        c.finishLink(false,ScaleDisconnectReason::HOST_RESET,0);
      } else {
        c.onGapEvent(&event,operation);
        testTerminateStatus=BLE_HS_EBUSY;
        c.service(); CHECK(testTerminations==1);
        for (unsigned retry=0;retry<8;++retry) c.service();
        CHECK(testTerminations==1);
        testNowMs+=SCALE_DISCONNECT_SILENCE_MS;
        testTerminateStatus=0;
        c.service(); CHECK(testTerminations==2);
        event.type=BLE_GAP_EVENT_DISCONNECT;
        event.disconnect.conn.conn_handle=42;
        c.onGapEvent(&event,operation);
        c.service();
        CHECK(!c.linkCleanupPending());
      }
    }
  }
  {
    NimbleScaleClient c(false); ready(c);
    for (unsigned cycle=0;cycle<3;++cycle) {
      c.connectionHandle_=kInvalidHandle;
      c.lifecycleActive_=true;
      c.enterState(NimbleScaleClient::State::Connecting);
      const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
      c.disconnect();
      ble_gap_event failed={}; failed.type=BLE_GAP_EVENT_CONNECT;
      failed.connect.status=BLE_HS_ETIMEOUT;
      c.onGapEvent(&failed,operation);
      CHECK(!c.linkCleanupPending());
      CHECK(testTerminations==0);
    }
    CHECK(c.cleanupCount_==3);
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(testNowMs==1100 && testTerminations==1);
    CHECK(c.linkCleanupPending() && c.connectionHandle_==kInvalidHandle);
    testNowMs=4000;
    ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
    gone.disconnect.conn.conn_handle=1; gone.disconnect.reason=BLE_HS_HCI_ERR(0x15);
    c.onGapEvent(&gone,operation);
    CHECK(c.linkCleanupPending()); // The callback only publishes completion.
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    CHECK(c.diagnostics().disconnectReason==static_cast<uint8_t>(
        ScaleDisconnectReason::COMMAND_WRITE_FAILED));
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_ETIMEOUT);
    const auto logsBefore=capturedScaleLogs.size();
    c.service();
    CHECK(!c.linkCleanupPending() && c.cleanupCount_==1);
    CHECK(capturedScaleLogs.size()==logsBefore+1);
    CHECK(capturedScaleLogs.back().second=="ble teardown complete raw=533 at_ms=4000");
    CHECK(capturedScaleLogTransport.back()); // BLE stack lines carry the ble hint.
    c.onGapEvent(&gone,operation); // Duplicate completion cannot extend silence.
    testNowMs=4100;
    c.service(); CHECK(c.scanStarts_==0 && testTerminations==1);
    testNowMs=6999;
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    testNowMs=7000;
    c.service(); CHECK(c.scanStarts_==1);
    CHECK(c.diagnostics().disconnectSequence==1);
    CHECK(c.diagnostics().commandStatus==BLE_HS_ETIMEOUT);
  }
  for (int terminationStatus : {0,BLE_HS_EALREADY}) {
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    testTerminateStatus=terminationStatus;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS*3;
    for (unsigned retry=0;retry<8;++retry) {
      c.service();
      CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    }
    CHECK(testTerminations==1 && c.scanStarts_==0 && c.linkCleanupPending());
    c.disconnect(); c.disconnect(); // Repeated user cleanup keeps the obligation.
    CHECK(c.linkCleanupPending() && c.cleanupCount_==1);
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
    ++testSyncGeneration;
    c.service(); CHECK(!c.linkCleanupPending());
    c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=testSyncGeneration;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t replacement=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    ble_gap_event event={}; event.type=BLE_GAP_EVENT_CONNECT; event.connect.conn_handle=1;
    c.onGapEvent(&event,replacement);
    event.type=BLE_GAP_EVENT_DISCONNECT; event.disconnect.conn.conn_handle=1;
    c.onGapEvent(&event,operation);
    CHECK(c.connectionHandle_==1 && !c.pendingDisconnect_ && !c.linkCleanupPending());
    CHECK(testTerminations==1);
    c.finishLink(false,ScaleDisconnectReason::HOST_RESET,0);
  }
  for (int terminationStatus : {BLE_HS_EBUSY,BLE_HS_EAGAIN,BLE_HS_ENOMEM}) {
    NimbleScaleClient c(false); ready(c);
    testTerminateStatus=terminationStatus;
    c.disconnect();
    CHECK(c.linkCleanupPending() && testTerminations==1);
    for (unsigned retry=0;retry<8;++retry) c.service();
    CHECK(testTerminations==1);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    c.service(); CHECK(testTerminations==1);
    ++testNowMs;
    testTerminateStatus=0;
    c.service(); CHECK(testTerminations==2 && c.linkCleanupPending());
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS*2;
    c.service(); CHECK(testTerminations==2);
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
  }
  {
    NimbleScaleClient c(false); ready(c);
    testTerminateStatus=BLE_HS_ENOTCONN;
    c.disconnect();
    CHECK(!c.linkCleanupPending() && testTerminations==1);
    CHECK(c.diagnostics().teardownStatus==BLE_HS_ENOTCONN);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS;
    CHECK(c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,BLE_SCAN_BALANCED_WINDOW,false));
  }
  for (bool earlyGap : {false,true}) {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::WriteFailed);
    CHECK(testNowMs==1100 && testTerminations==0 && c.linkCleanupPending());
    testNowMs=earlyGap ? 1600 : 3099;
    c.service(); CHECK(testTerminations==0);
    if (!earlyGap) {
      ++testNowMs;
      c.service(); CHECK(testTerminations==1 && c.scanStarts_==0);
      testNowMs+=SCALE_DISCONNECT_SILENCE_MS*2;
      c.service(); CHECK(testTerminations==1 && c.scanStarts_==0);
    }
    ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
    gone.disconnect.conn.conn_handle=1; gone.disconnect.reason=BLE_HS_HCI_ERR(0x15);
    c.onGapEvent(&gone,operation);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    c.service(); CHECK(!c.linkCleanupPending());
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_ETIMEOUT);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    c.service(); CHECK(c.scanStarts_==0);
    ++testNowMs;
    c.service(); CHECK(c.scanStarts_==1);
  }
  {
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    testOnTerminate=[&] {
      ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
      gone.disconnect.conn.conn_handle=1; gone.disconnect.reason=BLE_HS_HCI_ERR(0x15);
      c.onGapEvent(&gone,operation);
      testNowMs+=100; // The full quiet interval starts when submission returns.
    };
    c.disconnect();
    CHECK(testTerminations==1 && c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    CHECK(c.diagnostics().silenceTrigger==static_cast<uint8_t>(NimbleScaleClient::SilenceTrigger::Gap));
    c.service(); CHECK(!c.linkCleanupPending() && testTerminations==1);
    testOnTerminate={};
  }
  {
    NimbleScaleClient c(false); ready(c);
    ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
    gone.disconnect.conn.conn_handle=1; gone.disconnect.reason=BLE_HS_HCI_ERR(0x15);
    c.onGapEvent(&gone,c.linkOperationId_);
    c.disconnect();
    CHECK(!c.linkCleanupPending() && testTerminations==0);
    CHECK(c.lastReason()==ScaleDisconnectReason::USER_REQUEST);
  }
  {
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    c.disconnect();
    ble_gap_event gone={}; gone.type=BLE_GAP_EVENT_DISCONNECT;
    gone.disconnect.conn.conn_handle=1; gone.disconnect.reason=BLE_HS_HCI_ERR(0x15);
    c.onGapEvent(&gone,operation);
    CHECK(c.linkCleanupGapReceived_);
    ++testSyncGeneration;
    c.service();
    CHECK(!c.linkCleanupPending() && !c.linkCleanupGapReceived_);
    for (const auto &log : capturedScaleLogs) {
      CHECK(log.second.find("ble teardown complete")==std::string::npos);
    }
  }
  {
    NimbleScaleClient c(true); ready(c); // The protocol-ready line is a scale line.
    CHECK(!capturedScaleLogs.empty() && capturedScaleLogs.back().second.find("ready: ")==0);
    CHECK(!capturedScaleLogTransport.back());
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    ScaleProtocol heartbeat=*c.protocol_;
    heartbeat.features=kScaleProtocolAcaia.features;
    heartbeat.encodeCommand=kScaleProtocolAcaia.encodeCommand;
    c.protocol_=&heartbeat;
    c.lastHeartbeat_=nowMs()-heartbeat.features.heartbeatPeriodMs;
    testSubmitStatus=BLE_HS_EBUSY;
    for (unsigned attempt=0;attempt<4;++attempt) {
      CHECK(c.heartbeatRequired());
      CHECK(c.writeOp(ScaleOp::Heartbeat)==ScaleCommandResult::WriteFailed);
      CHECK(!c.heartbeatRequired());
      notify(c,20); CHECK(c.newWeightAvailable());
      CHECK(c.isLinkUp());
      testNowMs+=heartbeat.features.heartbeatPeriodMs;
    }
    const uint32_t limit=c.maxPacketPeriodMs();
    while (nowMs()-c.lastPacket_<limit) {
      if (c.heartbeatRequired()) {
        CHECK(c.writeOp(ScaleOp::Heartbeat)==ScaleCommandResult::WriteFailed);
      }
      CHECK(!c.newWeightAvailable());
      testNowMs+=heartbeat.features.heartbeatPeriodMs;
    }
    CHECK(!c.newWeightAvailable());
    CHECK(c.lastReason()==ScaleDisconnectReason::PACKET_TIMEOUT);
    CHECK(!c.isLinkUp());
  }
  for (unsigned iteration=0;iteration<32;++iteration) {
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    const uint32_t operation=c.linkOperationId_;
    std::atomic<bool> started{false}, stop{false};
    std::thread callback([&] {
      os_mbuf b={}; b.length=20; b.data[0]=3; b.data[1]=0x0b; b.data[19]=8;
      started.store(true,std::memory_order_release);
      for (unsigned packet=0;packet<8 && !stop.load(std::memory_order_acquire);++packet) {
        c.onNotification(1,10,&b,operation);
      }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Ok);
    stop.store(true,std::memory_order_release);
    callback.join();
    const uint32_t sequence=c.notificationSequence();
    notify(c,20);
    CHECK(c.notificationSequence()==sequence);
    CHECK(c.closedCommandGeneration_==c.generation_);
  }
  {
    // A timer-only stream after the first weight still times out.
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    ScaleProtocol timerOnly=*c.protocol_;
    timerOnly.supportedPacketLength=[](int length) { return length==19 || length==20; };
    timerOnly.parseTimer=[](const uint8_t *, int length, uint32_t *timerMs) {
      *timerMs=0;
      return length==19;
    };
    c.protocol_=&timerOnly;
    notify(c,20); CHECK(c.newWeightAvailable());
    const uint32_t limit=c.maxPacketPeriodMs();
    for (uint32_t waited=0; waited<limit; waited+=500) {
      testNowMs+=500;
      notify(c,19);
      CHECK(!c.newWeightAvailable());
    }
    CHECK(!c.isConnected());
    CHECK(c.lastReason()==ScaleDisconnectReason::PACKET_TIMEOUT);
  }
  for (bool first : {false, true}) {
    for (bool stale : {false, true}) {
      for (bool wrap : {false, true}) {
        testNowMs=wrap ? UINT32_MAX-2000ULL : 100;
        NimbleScaleClient c(false); ready(c);
        if (!first) { notify(c,20); CHECK(c.newWeightAvailable()); }
        const uint32_t limit=first ? FIRST_PACKET_TIMEOUT_MS : 8000;
        testNowMs+=stale ? 0 : limit-1;
        notify(c,20);
        testNowMs+=stale ? limit : 1;
        CHECK(c.newWeightAvailable()==!stale);
        CHECK(c.isConnected()==!stale);
        if (stale) CHECK(c.lastReason()==(first
            ? ScaleDisconnectReason::FIRST_PACKET_TIMEOUT
            : ScaleDisconnectReason::PACKET_TIMEOUT));
        else CHECK(c.lastPacketAgeMs()==1);
      }
    }
  }
  for (bool first : {false, true}) {
    for (unsigned invalid=0; invalid<3; ++invalid) {
      testNowMs=100;
      NimbleScaleClient c(false); ready(c);
      if (!first) { notify(c,20); CHECK(c.newWeightAvailable()); }
      const uint32_t limit=first ? FIRST_PACKET_TIMEOUT_MS : 8000;
      testNowMs+=limit-1;
      if (invalid==0) notify(c,19); // Fresh but truncated.
      if (invalid==1) {
        os_mbuf b={}; b.length=20; b.data[0]=3; b.data[1]=0x0b; b.data[19]=8;
        c.onNotification(1,10,&b,c.linkOperationId_+1);
      }
      ++testNowMs;
      if (invalid==2) notify(c,20); // First valid frame arrived too late.
      CHECK(!c.newWeightAvailable()); CHECK(!c.isConnected());
      CHECK(c.lastReason()==(first ? ScaleDisconnectReason::FIRST_PACKET_TIMEOUT
                                  : ScaleDisconnectReason::PACKET_TIMEOUT));
    }
  }
  {
    NimbleScaleClient c(false); ready(c);
    os_mbuf b={}; b.length=20; b.data[0]=3; b.data[1]=0x0b;
    b.data[3]=0x12; b.data[4]=0x34;
    b.data[6]='-'; b.data[8]=0x1f; b.data[9]=0x40;
    for (unsigned i=0;i<19;++i) b.data[19]^=b.data[i];
    for (unsigned invalid=0;invalid<2;++invalid) {
      b.data[invalid ? 19 : 1]^=1;
      if (!invalid) b.data[19]^=1; // Wrong type with an otherwise valid checksum.
      c.onNotification(1,10,&b,c.linkOperationId_);
      CHECK(!c.newWeightAvailable());
      CHECK(!c.hasValidPacket_);
      b.data[invalid ? 19 : 1]^=1;
      if (!invalid) b.data[19]^=1;
    }
    c.onNotification(1,10,&b,c.linkOperationId_);
    CHECK(c.newWeightAvailable()); CHECK(c.weight()==-80.0f);
    CHECK(c.currentTimerMs_==0x1234);
    CHECK(c.rejectedPackets()==2);
    const uint8_t codes[]={1,4,5,6,7,2,0x15}; unsigned command=0;
    for (ScaleOp op : {ScaleOp::Tare,ScaleOp::StartTimer,ScaleOp::StopTimer,
                      ScaleOp::ResetTimer,ScaleOp::CombinedTareStart,ScaleOp::SetVolume,
                      ScaleOp::PowerOff}) {
      uint8_t payload[SCALE_MAX_COMMAND_LENGTH]={}; int length=0;
      CHECK(kScaleProtocolGenericFf11.encodeCommand(op,3,payload,&length));
      CHECK(length==6); uint8_t sum=0;
      CHECK(payload[0]==3 && payload[1]==0x0a && payload[2]==codes[command++]);
      CHECK(payload[3]==0 && payload[4]==(op==ScaleOp::SetVolume ? 3 : 0));
      for (int i=0;i<length-1;++i) sum^=payload[i];
      CHECK(payload[length-1]==sum);
    }
    CHECK(kScaleProtocolGenericFf11.features.has(ScaleFeaturePowerOff));
    CHECK(!kScaleProtocolAcaia.features.has(ScaleFeaturePowerOff));
    CHECK(!kScaleProtocolDifluid.features.has(ScaleFeaturePowerOff));
    // Heartbeats land inside each scale's keep-alive timeout with margin.
    CHECK(kScaleProtocolDecent.features.heartbeatPeriodMs <= 5000 * 4 / 5);
    CHECK(kScaleProtocolDifluid.features.heartbeatPeriodMs <= 5000 * 4 / 5);
    CHECK(!kScaleProtocolFelicita.features.has(ScaleFeaturePowerOff));
  }
  {
    NimbleScaleClient c(false);
    c.beginGeneration();
    c.state_ = NimbleScaleClient::State::Scanning;
    const uint32_t operation = c.beginOperation(NimbleScaleClient::CallbackDomain::Scan);
    uint8_t firstName[] = "BOOKOO first";
    uint8_t secondName[] = "BOOKOO second";
    testAdvertisementParseStatus = 0;
    testAdvertisementFields = {firstName, sizeof(firstName) - 1, 1, 0, nullptr};
    ble_gap_disc_desc first = {{0, {1, 2, 3, 4, 5, 6}},
                              BLE_HCI_ADV_RPT_EVTYPE_ADV_IND, nullptr, 0};
    c.onAdvertisement(first, operation);
    char mac[32] = {};
    char name[32] = {};
    // Publication after the consumption lock releases must remain pending,
    // while the first call returns its coherent address/name pair.
    testAfterCriticalExit = [&] {
      testAdvertisementFields.name = secondName;
      testAdvertisementFields.name_len = sizeof(secondName) - 1;
      first.addr.val[0] = 7;
      c.onAdvertisement(first, operation);
    };
    CHECK(c.takeSeenAdvertisement(mac, sizeof(mac), name, sizeof(name)));
    CHECK(strcmp(mac, "06:05:04:03:02:01") == 0);
    CHECK(strcmp(name, "BOOKOO first") == 0);
    CHECK(c.takeSeenAdvertisement(mac, sizeof(mac), name, sizeof(name)));
    CHECK(strcmp(mac, "06:05:04:03:02:07") == 0);
    CHECK(strcmp(name, "BOOKOO second") == 0);
    CHECK(!c.takeSeenAdvertisement(mac, sizeof(mac), name, sizeof(name)));
    c.onAdvertisement(first, operation + 1);
    CHECK(!c.takeSeenAdvertisement(mac, sizeof(mac), name, sizeof(name)));
    // Latest identity survives candidate-slot eviction and small destinations
    // keep the public truncation/termination behavior.
    for (unsigned i = 0; i < kCandidateCount + 2; ++i) {
      first.addr.val[0] = static_cast<uint8_t>(i);
      c.onAdvertisement(first, operation);
    }
    CHECK(c.takeSeenAdvertisement(mac, 3, name, 1));
    CHECK(strcmp(mac, "06") == 0 && name[0] == '\0');
    c.onAdvertisement(first, operation);
    CHECK(c.takeSeenAdvertisement(nullptr, 0, nullptr, 0));
    CHECK(!c.seenPending_ && testCriticalDepth == 0);
    testAdvertisementParseStatus = BLE_HS_EINVAL;
    testAdvertisementFields = {};
  }
  {
    NimbleScaleClient c(false);
    CHECK(c.beginConfiguredScan(false));
    c.enterState(NimbleScaleClient::State::Backoff, 1);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    CHECK(!c.candidatePending_);
    c.enterState(NimbleScaleClient::State::Scanning);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_NONCONN_IND);
    CHECK(!c.candidatePending_);

    c.clearScanData();
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND, "");
    CHECK(!c.candidatePending_);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP);
    CHECK(c.candidatePending_);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.finishLink(false, ScaleDisconnectReason::REMOTE_DISCONNECTED,
                 BLE_HS_HCI_ERR(0x13));
    CHECK(capturedScaleLogs.back().first == kScaleLogInfoSeverity);
    CHECK(capturedScaleLogs.back().second.find("ble error op=link") == 0);
    CHECK(capturedScaleLogs.back().second.find("reason=remote disconnected") !=
          std::string::npos);
    testNowMs = c.backoff_.deadlineMs();
    c.service();
    CHECK(c.state_ == NimbleScaleClient::State::Scanning);
    CHECK(c.connectAttemptsTotal_ == 0);
  }
  for (const int status : {BLE_HS_HCI_ERR(0x08), BLE_HS_HCI_ERR(0x15)}) {
    NimbleScaleClient c(false); ready(c);
    c.activeCommand_ = status == BLE_HS_HCI_ERR(0x15) ? 0 : 0xff;
    c.finishLink(false, mapRawDisconnectReason(status), status);
    CHECK(capturedScaleLogs.back().first == kScaleLogWarningSeverity);
    CHECK(c.lastReason() != ScaleDisconnectReason::REMOTE_DISCONNECTED ||
          capturedScaleLogs.back().second.find("reason=scale powered off") !=
              std::string::npos);
  }
  {
    NimbleScaleClient c(false);
    CHECK(c.beginConfiguredScan(false));
    for (unsigned attempt = 0; attempt < 12; ++attempt) {
      if (c.state_ == NimbleScaleClient::State::Backoff) {
        testNowMs = c.backoff_.deadlineMs();
        c.service();
      }
      advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
      c.service();
      testNowMs += SCALE_CONNECT_SETTLE_MS;
      c.service();
      CHECK(c.state_ == NimbleScaleClient::State::Backoff);
    }
    CHECK(c.connectAttemptsTotal_ == 12);
    CHECK(c.backoff_.failureCount() == 12);
    testNowMs = c.backoff_.deadlineMs();
    c.service();
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    c.service();
    CHECK(c.connectAttemptsTotal_ == 13);
    CHECK(c.state_ == NimbleScaleClient::State::Backoff);
  }
  {
    NimbleScaleClient c(false); ready(c);
    testOnSubmit=[] { xTaskNotifyGive(xTaskGetCurrentTaskHandle()); };
    testOnWait=[] { complete(); };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(testTerminations==0);
    CHECK(testWakeCount==1); // ATT must not consume the worker's wakeup.
  }
  {
    NimbleScaleClient c(false); ready(c);
    testOnSubmit=[] { complete(); };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    notify(c,20);
    const uint32_t linkOperation=c.linkOperationId_;
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.reason=BLE_HS_HCI_ERR(8); e.disconnect.conn.conn_handle=1;
    c.onGapEvent(&e,linkOperation);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    CHECK(c.rxCount_==0);
    CHECK(c.diagnostics().silenceTrigger==static_cast<uint8_t>(
        NimbleScaleClient::SilenceTrigger::Gap));
    const unsigned callsAtDisconnect=testRadioProcedures;
    bool invoked=false;
    CHECK(c.submitRadioProcedure(false,[&] { invoked=true; return 0; })==
          BLE_HS_EBUSY);
    CHECK(!invoked);
    CHECK(c.rssi()==SCALE_LINK_RSSI_UNAVAILABLE);
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::NotConnected);
    CHECK(testRadioProcedures==callsAtDisconnect);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    CHECK(!c.startScan(nullptr,false,BLE_SCAN_BALANCED_INTERVAL,
                       BLE_SCAN_BALANCED_WINDOW,false));
    CHECK(testRadioProcedures==callsAtDisconnect);
    ++testNowMs;
    CHECK(!c.communicationSilenced());
    CHECK(c.beginConfiguredScan(false));
    CHECK(testRadioProcedures==callsAtDisconnect+1);
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false);
    testRuntimeReady=true; testSyncGeneration=1;
    c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=1;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_CONNECT;
    e.connect.conn_handle=42;
    c.onGapEvent(&e,operation);
    e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.conn.conn_handle=42;
    e.disconnect.reason=BLE_HS_HCI_ERR(8);
    c.onGapEvent(&e,operation);
    CHECK(c.pendingDisconnect_);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    const unsigned procedures=testRadioProcedures;
    c.service();
    CHECK(c.lastReason()==ScaleDisconnectReason::SUPERVISION_TIMEOUT);
    CHECK(testRadioProcedures==procedures);
  }
  {
    NimbleScaleClient c(false);
    testRuntimeReady=true; testSyncGeneration=1;
    c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=1;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t oldOperation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_CONNECT;
    e.connect.conn_handle=42;
    c.onGapEvent(&e,oldOperation);
    CHECK(c.connectionHandle_==42);
    e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.conn.conn_handle=43;
    c.onGapEvent(&e,oldOperation);
    CHECK(!c.pendingDisconnect_);
    testSyncGeneration=2;
    c.service();
    CHECK(c.lastReason()==ScaleDisconnectReason::HOST_RESET);
    CHECK(c.connectionHandle_==kInvalidHandle);
    c.beginGeneration(); c.lifecycleActive_=true; c.syncGeneration_=2;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t newOperation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    e.disconnect.conn.conn_handle=42;
    c.onGapEvent(&e,oldOperation);
    CHECK(!c.pendingDisconnect_);
    e.type=BLE_GAP_EVENT_CONNECT;
    e.connect.conn_handle=42;
    c.onGapEvent(&e,newOperation);
    e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.conn.conn_handle=42;
    c.onGapEvent(&e,newOperation);
    CHECK(c.pendingDisconnect_);
  }
  {
    NimbleScaleClient c(false);
    c.beginGeneration(); c.lifecycleActive_=true;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_CONNECT;
    e.connect.status=BLE_HS_HCI_ERR(8); e.connect.conn_handle=42;
    c.onGapEvent(&e,operation);
    CHECK(c.connectionHandle_==kInvalidHandle);
  }
  {
    testNowMs=UINT32_MAX-500ULL;
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Ok);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    CHECK(c.communicationSilenceRemainingMs()==1);
    ++testNowMs;
    CHECK(!c.communicationSilenced());
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Ok);
    testNowMs+=500;
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.reason=BLE_HS_HCI_ERR(8); e.disconnect.conn.conn_handle=1;
    c.onGapEvent(&e,c.linkOperationId_);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    CHECK(c.communicationSilenced());
    ++testNowMs;
    CHECK(!c.communicationSilenced());
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    bool nestedSubmitted=false;
    CHECK(c.submitRadioProcedure(false,[&] {
      CHECK(c.submitRadioProcedure(false,[&] {
        nestedSubmitted=true;
        return 0;
      })==BLE_HS_EBUSY);
      return 0;
    })==0);
    CHECK(!nestedSubmitted);
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    const uint32_t operation=c.linkOperationId_;
    testAfterCriticalExit=[&] {
      ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
      e.disconnect.reason=BLE_HS_HCI_ERR(8);
      e.disconnect.conn.conn_handle=1;
      c.onGapEvent(&e,operation);
    };
    bool silencedDuringSubmit=false;
    CHECK(c.submitRadioProcedure(false,[&] {
      silencedDuringSubmit=c.communicationSilenced();
      bool nestedSubmitted=false;
      CHECK(c.submitRadioProcedure(false,[&] {
        nestedSubmitted=true;
        return 0;
      })==BLE_HS_EBUSY);
      CHECK(!nestedSubmitted);
      return 0;
    })==0);
    CHECK(!silencedDuringSubmit);
    CHECK(c.pendingDisconnect_);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    testAfterCriticalExit={};
  }
  {
    NimbleScaleClient c(false); ready(c);
    const int raw=BLE_HS_HCI_ERR(8);
    CHECK(c.armGapSilenceIfCurrent(c.linkOperationId_,1,raw));
    c.finishLink(true,ScaleDisconnectReason::DISCOVERY_FAILED,BLE_HS_EBUSY);
    CHECK(c.lastReason()==ScaleDisconnectReason::SUPERVISION_TIMEOUT);
    CHECK(c.diagnostics().disconnectStatus==raw);
    CHECK(testTerminations==0);
  }
  {
    testNowMs=100;
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    ScaleProtocol paced=*c.protocol_;
    paced.features.minimumCommandIntervalMs=100;
    c.protocol_=&paced;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    const uint32_t linkOperation=c.linkOperationId_;
    testAfterCriticalExit=[&] {
      ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
      e.disconnect.reason=BLE_HS_HCI_ERR(8); e.disconnect.conn.conn_handle=1;
      c.onGapEvent(&e,linkOperation);
    };
    CHECK(c.writeOp(ScaleOp::StopTimer)!=ScaleCommandResult::Ok);
    CHECK(testWrites==1);
    CHECK(c.communicationSilenced());
    testAfterCriticalExit={};
  }
  {
    NimbleScaleClient c(false); ready(c);
    ScaleProtocol timerOnly=*c.protocol_;
    timerOnly.parseWeight=[](const uint8_t *,int,float *) { return false; };
    timerOnly.parseTimer=[](const uint8_t *,int,uint32_t *timer) {
      *timer=0;
      return true;
    };
    c.protocol_=&timerOnly;
    for (size_t i=0;i<kRxFrameCount;++i) notify(c,20);
    size_t replenished=0;
    std::function<void()> replenish;
    replenish=[&] {
      if (c.rxCount_<kRxFrameCount) {
        testAfterCriticalExit={};
        notify(c,20);
        ++replenished;
      }
      testAfterCriticalExit=replenish;
    };
    testAfterCriticalExit=replenish;
    CHECK(!c.newWeightAvailable());
    testAfterCriticalExit={};
    CHECK(replenished==kRxFrameCount);
    CHECK(c.rxCount_==kRxFrameCount);
  }
  for (const int status : {BLE_HS_EBUSY, BLE_HS_EAGAIN, BLE_HS_ENOMEM}) {
    NimbleScaleClient c(false); ready(c);
    testSubmitStatus=status;
    CHECK(c.writeOp(ScaleOp::SetVolume)==ScaleCommandResult::WriteFailed);
    CHECK(c.isConnected()); CHECK(testTerminations==0);
    CHECK(c.diagnostics().commandStatus==status);
    CHECK(c.diagnostics().disconnectSequence==0);
    testSubmitStatus=0; testOnSubmit=[] { complete(); };
    CHECK(c.writeOp(ScaleOp::StopTimer)==ScaleCommandResult::Ok);
    CHECK(c.diagnostics().commandStatus==status);
  }
  {
    NimbleScaleClient c(false); ready(c);
    testOnWait=[] { complete(BLE_HS_ATT_ERR(3)); };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(c.isConnected()); CHECK(testTerminations==0);
    CHECK(c.diagnostics().commandStatus==BLE_HS_ATT_ERR(3));
    CHECK(capturedScaleLogs.size()==3);
    CHECK(capturedScaleLogs[1].first==kScaleLogWarningSeverity);
    CHECK(capturedScaleLogs[1].second.find(
        "op=tare domain=att raw=259 hex=0x103 code=0x03")!=std::string::npos);
  }
  for (const bool response : {false,true}) {
    NimbleScaleClient c(false); ready(c);
    if (!response) c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    testSubmitStatus=BLE_HS_EUNKNOWN;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(!c.isConnected()); CHECK(testTerminations==1);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    testSubmitStatus=BLE_HS_ENOMEM;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(c.isConnected()); CHECK(testTerminations==0);
  }
  for (const bool response : {false,true}) {
    NimbleScaleClient c(false); ready(c);
    ScaleProtocol paced=*c.protocol_;
    paced.features.minimumCommandIntervalMs=100;
    c.protocol_=&paced;
    if (!response) c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    else testOnSubmit=[] { complete(); };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    const uint64_t firstAt=testNowMs;
    CHECK(c.writeOp(ScaleOp::StopTimer)==ScaleCommandResult::Ok);
    CHECK(testNowMs-firstAt==100); CHECK(testWrites==2);
    CHECK(capturedScaleLogs.size()==4);
    CHECK(capturedScaleLogs[2].second.find(response
        ? "ble tx command/stop_timer response=1 bytes=03 0A 05 00 00 0C"
        : "ble tx command/stop_timer response=0 bytes=03 0A 05 00 00 0C")!=std::string::npos);
    CHECK(capturedScaleLogs[3].second.find("gap_ms=100")!=std::string::npos);
  }
  for (bool response : {false, true}) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      NimbleScaleClient c(false); ready(c);
      if (response) testOnSubmit=[] { complete(); };
      else c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
      CHECK(c.writeOp(ScaleOp::StopTimer)==ScaleCommandResult::Ok);
      const uint64_t previousWriteAt = testNowMs;
      const uint32_t beforeSpacing = c.notificationSequence();
      bool notified = false;
      testOnDelay = [&] {
        if (!notified) { notify(c,20); notified=true; }
      };
      struct Context { NimbleScaleClient *client; unsigned mode; uint64_t dueAt; uint32_t oldBoundary; };
      Context context{&c, mode, previousWriteAt + 100, beforeSpacing};
      const ScaleCommandAdmission admission{
        [](void *opaque, uint32_t &boundary) {
          auto &ctx = *static_cast<Context *>(opaque);
          CHECK(testCriticalDepth==0);
          CHECK(testNowMs >= ctx.dueAt);
          CHECK(ctx.client->newWeightAvailable());
          boundary = ctx.client->notificationSequence();
          CHECK(boundary > ctx.oldBoundary); // Spacing-window zero is preflight data.
          if (ctx.mode==1) notify(*ctx.client,20); // Final admission must recheck it.
          return ctx.mode!=0;
        }, &context};
      const ScaleCommandResult result = c.writeOp(ScaleOp::Tare,0,&admission);
      CHECK(result==(mode==2 ? ScaleCommandResult::Ok : ScaleCommandResult::Deferred));
      CHECK(testWrites==(mode==2 ? 2U : 1U));
      CHECK(c.isConnected());
      CHECK(c.gattOperationId_==0);
      CHECK(c.diagnostics_.commandFailureSequence==0);
      testOnDelay={};
    }
  }
  {
    testNowMs=UINT32_MAX-50ULL;
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    const uint64_t firstAt=testNowMs;
    CHECK(c.writeOp(ScaleOp::StopTimer)==ScaleCommandResult::Ok);
    CHECK(testNowMs-firstAt==100); CHECK(testWrites==2);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.pendingDisconnect_=true;
    c.pendingDisconnectStatus_=BLE_HS_HCI_ERR(8);
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::NotConnected);
    CHECK(testWrites==0); CHECK(!c.isConnected());
    CHECK(c.lastReason()==ScaleDisconnectReason::SUPERVISION_TIMEOUT);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Ok);
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::NotConnected);
    CHECK(testWrites==1);
    CHECK(c.communicationSilenceRemainingMs()==SCALE_DISCONNECT_SILENCE_MS);
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS-1;
    CHECK(c.communicationSilenced());
    ++testNowMs;
    CHECK(!c.communicationSilenced());
    ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(capturedScaleLogs.size()==2);
    CHECK(capturedScaleLogs[0].first==kScaleLogInfoSeverity);
    CHECK(capturedScaleLogs[0].second.find("ble tx command/tare response=0 bytes=03 0A 01 00 00 08")!=std::string::npos);
    CHECK(capturedScaleLogs[1].second.find("submitted=1 result=ok raw=0")!=std::string::npos);
    CHECK(capturedScaleLogs[1].second.find("gap_ms=0")!=std::string::npos);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    testSubmitStatus=BLE_HS_EBUSY;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(capturedScaleLogs.size()==2);
    CHECK(capturedScaleLogs[0].first==kScaleLogWarningSeverity);
    CHECK(capturedScaleLogs[0].second.find("op=tare domain=host raw=15")!=std::string::npos);
    CHECK(capturedScaleLogs[1].first==kScaleLogInfoSeverity);
    CHECK(capturedScaleLogs[1].second.find(
        "submitted=0 result=failed raw=15")!=std::string::npos);
  }
  for (const int code : {0x0b, 0x1f}) {
    NimbleScaleClient c(false); ready(c);
    ble_gap_event e={};
    if (code==0x0b) {
      c.enterState(NimbleScaleClient::State::Connecting);
      e.type=BLE_GAP_EVENT_CONNECT;
      e.connect.status=BLE_HS_HCI_ERR(code);
      e.connect.conn_handle=1;
    } else {
      e.type=BLE_GAP_EVENT_DISCONNECT;
      e.disconnect.reason=BLE_HS_HCI_ERR(code);
      e.disconnect.conn.conn_handle=1;
    }
    c.onGapEvent(&e,c.linkOperationId_);
    c.service(); c.service();
    CHECK(capturedScaleLogs.size()==1);
    CHECK(capturedScaleLogs[0].first==kScaleLogWarningSeverity);
    CHECK(capturedScaleLogs[0].second.find(
        code==0x0b ? "op=connect domain=hci" : "op=link domain=hci")!=std::string::npos);
    CHECK(capturedScaleLogs[0].second.find(
        code==0x0b ? "raw=523 hex=0x20B code=0x0B" :
                     "raw=543 hex=0x21F code=0x1F")!=std::string::npos);
  }
  {
    NimbleScaleClient c(false); ready(c);
    testRssiStatus=BLE_HS_HCI_ERR(0x1f);
    CHECK(c.rssi()==SCALE_LINK_RSSI_UNAVAILABLE);
    CHECK(c.isConnected());
    CHECK(capturedScaleLogs.size()==1);
    CHECK(capturedScaleLogs[0].first==kScaleLogWarningSeverity);
    CHECK(capturedScaleLogs[0].second.find(
        "op=rssi domain=hci raw=543 hex=0x21F code=0x1F")!=std::string::npos);
  }
  {
    NimbleScaleClient c(false); ready(c);
    const uint8_t cccd[] = {1, 0};
    CHECK(c.submitWrite(13, cccd, sizeof(cccd),
                        NimbleScaleClient::WritePurpose::Subscribe,
                        "enable_notify", true));
    CHECK(capturedScaleLogs.size()==1);
    CHECK(capturedScaleLogs[0].second.find(
        "ble tx subscribe/enable_notify response=1 bytes=01 00")!=std::string::npos);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.protocol_=&kScaleProtocolAcaia;
    c.initWriteIndex_=0;
    c.beginNextInitWrite();
    CHECK(capturedScaleLogs.size()==1);
    CHECK(capturedScaleLogs[0].second.find(
        "ble tx initialize/identify response=1 bytes=EF DD 0B 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 9A 6D")!=std::string::npos);
  }
  {
    NimbleScaleClient c(false); ready(c);
    const uint64_t before=testNowMs;
    testOnWait=[&] { testNowMs+=3; xSemaphoreGive(c.writeSignal_); };
    testTerminateStatus=BLE_HS_ENOTCONN;
    CHECK(c.writeOp(ScaleOp::CombinedTareStart)==ScaleCommandResult::WriteFailed);
    CHECK(testNowMs-before>=BLE_OPERATION_TIMEOUT_MS);
    CHECK(testNowMs-before<=BLE_OPERATION_TIMEOUT_MS+3);
    CHECK(testTerminations==1); CHECK(testWrites==1);
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_ETIMEOUT);
    CHECK(c.diagnostics().teardownStatus==BLE_HS_ENOTCONN);
    CHECK(c.diagnostics().disconnectCommand==static_cast<uint8_t>(ScaleOp::CombinedTareStart));
    const auto oldCallback=testWriteCallback; void *oldArg=testWriteArg;
    testNowMs+=SCALE_DISCONNECT_SILENCE_MS;
    ready(c);
    testOnWait=[&] {
      ble_gatt_error e={0,12}; oldCallback(1,&e,nullptr,oldArg);
      CHECK(!c.writeCompleted_);
      complete();
    };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_ETIMEOUT);
  }
  for (const bool completionFirst : {false,true}) {
    NimbleScaleClient c(false); ready(c);
    testOnWait=[&] {
      if (completionFirst) complete();
      ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
      e.disconnect.reason=BLE_HS_HCI_ERR(8); e.disconnect.conn.conn_handle=1;
      c.onGapEvent(&e,c.linkOperationId_);
    };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(c.lastReason()==ScaleDisconnectReason::SUPERVISION_TIMEOUT);
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_HCI_ERR(8));
    CHECK(c.diagnostics().disconnectOrigin==ScaleBleDisconnectOrigin::Gap);
    CHECK(c.diagnostics().disconnectSequence==1); CHECK(testTerminations==0);
    c.service(); CHECK(c.diagnostics().disconnectSequence==1);
  }
  {
    NimbleScaleClient c(false); ready(c);
    c.activeCommand_=static_cast<uint8_t>(ScaleOp::Tare);
    c.commandStartedAt_=nowMs();
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_DISCONNECT;
    e.disconnect.reason=BLE_HS_HCI_ERR(8); e.disconnect.conn.conn_handle=1;
    c.onGapEvent(&e,c.linkOperationId_);
    c.finishLink(true,ScaleDisconnectReason::COMMAND_WRITE_FAILED,BLE_HS_ETIMEOUT);
    CHECK(c.lastReason()==ScaleDisconnectReason::SUPERVISION_TIMEOUT);
    CHECK(c.diagnostics().disconnectStatus==BLE_HS_HCI_ERR(8));
    CHECK(testTerminations==0);
  }
  {
    NimbleScaleClient c(false); ready(c);
    testOnWait=[] { testRuntimeReady=false; ++testSyncGeneration; };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(c.lastReason()==ScaleDisconnectReason::HOST_RESET);
    CHECK(c.diagnostics().disconnectSequence==1);
    CHECK(testTerminations==0);
  }
  {
    NimbleScaleClient c(false); ready(c);
    testNowMs=UINT32_MAX-25ULL;
    const uint64_t before=testNowMs;
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::WriteFailed);
    CHECK(testNowMs-before==BLE_OPERATION_TIMEOUT_MS);
  }
  {
    NimbleScaleClient c(false); ready(c);
    for (unsigned i=0;i<8;++i) {
      notify(c,21); notify(c,20); CHECK(c.newWeightAvailable());
    }
    CHECK(c.isConnected()); CHECK(testTerminations==0);
    for (unsigned i=0;i<7;++i) notify(c,21);
    CHECK(c.isConnected());
    notify(c,21); CHECK(!c.isConnected()); CHECK(testTerminations==1);
  }
  {
    NimbleScaleClient c(false); ready(c);
    const uint32_t capturedAt = testNowMs;
    notify(c,20);
    const uint32_t beforeWrite = c.notificationSequence();
    testNowMs += 1200;
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().receivedAtMs == capturedAt);
    CHECK(c.weightSample().captureSequence == beforeWrite);
    CHECK(c.weightSample().weightG == 0.0f);
    testOnWait=[&] { notify(c,20); complete(); };
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().captureSequence == beforeWrite + 1);
    CHECK(c.weightSample().receivedAtMs >= capturedAt + 1200);
    c.notificationSequence_=UINT32_MAX;
    notify(c,20);
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().captureSequence == 1);
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    testOnWait={};
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(!c.newWeightAvailable()); // Transport success creates no weight evidence.
  }
  {
    // The sample carries the scale's flow only when the link declares it.
    NimbleScaleClient c(false); ready(c);
    os_mbuf b={}; b.length=20;
    const uint8_t frame[19]={3,0x0b,0,0,0,1,'+',0,0x0e,0x10,'-',0,0x9d};
    memcpy(b.data,frame,sizeof(frame));
    for (unsigned i=0;i<19;++i) b.data[19]^=b.data[i];
    c.onNotification(1,10,&b,c.linkOperationId_);
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().weightG==36.0f);
    CHECK(c.weightSample().flowRateValid && c.weightSample().flowRateGps==-1.57f);
    ScaleProtocol withoutFlag=*c.protocol_;  // Hook present, capability off.
    withoutFlag.features.flags&=~static_cast<uint32_t>(ScaleFeatureFlowRate);
    c.protocol_=&withoutFlag;
    c.onNotification(1,10,&b,c.linkOperationId_);
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().weightG==36.0f && !c.weightSample().flowRateValid);
    c.protocol_=&kScaleProtocolWeighMyBru;  // Same envelope, no flow capability.
    c.onNotification(1,10,&b,c.linkOperationId_);
    CHECK(c.newWeightAvailable());
    CHECK(c.weightSample().weightG==36.0f && !c.weightSample().flowRateValid);
  }
  {
    NimbleScaleClient c(false); ready(c);
    strcpy(c.name_, "BOOKOO_SC 715097"); c.identityPresent_=true;
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.model()==ScaleModel::BookooMini);
    CHECK(!c.features().has(ScaleFeaturePowerOff));
    CHECK(c.features().volumeMax==5);
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Unsupported);
    CHECK(testWrites==0 && c.closedCommandGeneration_!=c.generation_);
    CHECK(c.writeOp(ScaleOp::Tare)==ScaleCommandResult::Ok);
    CHECK(testWrites==1);
    c.enterState(NimbleScaleClient::State::Scanning);
    CHECK(c.model()==ScaleModel::Unknown);
    ready(c);
    strcpy(c.name_, "BOOKOO_SC U 90210"); c.identityPresent_=true;
    CHECK(c.model()==ScaleModel::BookooUltra);
    CHECK(c.features().volumeMax==3);
    strcpy(c.name_, "BOOKOO_SC_U 369041");
    CHECK(c.model()==ScaleModel::BookooUltra);
    CHECK(strcmp(scaleDefaultFriendlyName(c.name_),"BOOKOO Themis Ultra")==0);
    c.protocol_=&kScaleProtocolAcaia;
    CHECK(c.model()==ScaleModel::Unknown);
  }
  {
    NimbleScaleClient c(false); ready(c);
    strcpy(c.name_, "BOOKOO_SC_U 369041"); c.identityPresent_=true;
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.model()==ScaleModel::BookooUltra);
    CHECK(c.features().volumeMax==3);
    CHECK(c.writeOp(ScaleOp::SetVolume,4)==ScaleCommandResult::InvalidArgument);
    CHECK(c.writeOp(ScaleOp::SetVolume,5)==ScaleCommandResult::InvalidArgument);
    CHECK(testWrites==0 && c.closedCommandGeneration_!=c.generation_);
    CHECK(c.writeOp(ScaleOp::SetVolume,3)==ScaleCommandResult::Ok);
    CHECK(c.writeOp(ScaleOp::PowerOff)==ScaleCommandResult::Ok);
    CHECK(testWrites==2);
  }
  {
    NimbleScaleClient c(false); ready(c);
    strcpy(c.name_, "BOOKOO_SC U"); c.identityPresent_=true;
    c.writeProperties_=BLE_GATT_CHR_PROP_WRITE_NO_RSP;
    CHECK(c.model()==ScaleModel::Unknown);
    CHECK(c.features().volumeMax==5);
    CHECK(c.writeOp(ScaleOp::SetVolume,5)==ScaleCommandResult::Ok);
    c.identityPresent_=false;
    CHECK(c.model()==ScaleModel::Unknown);
  }
  {
    const uint8_t miniCodes[]={1,2,4,5,6,7};
    ScaleCommandInfo command={};
    for(size_t i=0;i<6;++i){
      CHECK(scaleBookooCommandAt(ScaleModel::BookooMini,i,&command));
      CHECK(command.code==miniCodes[i]);
    }
    CHECK(!scaleBookooCommandAt(ScaleModel::BookooMini,6,&command));
    CHECK(scaleBookooCommandAt(ScaleModel::BookooUltra,6,&command));
    CHECK(command.code==0x15);
    CHECK(!scaleBookooCommandAt(ScaleModel::Unknown,0,&command));
    CHECK(scaleBookooOpcode(ScaleOp::SetVolume)==0x02);
    CHECK(scaleModelForAdvertisement("BOOKOO_SC U nope")==ScaleModel::Unknown);
    CHECK(scaleModelForAdvertisement("BOOKOO_SC 123X")==ScaleModel::Unknown);
    for (const char *name : {"BOOKOO_SC_U", "BOOKOO_SC_U ",
                            "BOOKOO_SC_U nope", "BOOKOO_SC_U U 123",
                            "BOOKOO_SC_U 123X", "BOOKOO_SC_U 123 -"}) {
      CHECK(scaleModelForAdvertisement(name)==ScaleModel::Unknown);
    }
    const uint8_t ultraCodes[]={1,2,4,5,6,7,0x15};
    const ScaleModel ultra=scaleModelForAdvertisement("BOOKOO_SC_U 369041");
    for(size_t i=0;i<7;++i){
      CHECK(scaleBookooCommandAt(ultra,i,&command));
      CHECK(command.code==ultraCodes[i]);
    }
    CHECK(!scaleBookooCommandAt(ultra,7,&command));
    CHECK(strcmp(scaleDefaultFriendlyName("LUNAR"),"LUNAR")==0);
  }
  for (const auto preset : {std::make_pair(BLE_SCAN_RELAXED_INTERVAL,BLE_SCAN_RELAXED_WINDOW),
                            std::make_pair(BLE_SCAN_AGGRESSIVE_INTERVAL,BLE_SCAN_AGGRESSIVE_WINDOW)}) {
    // The discovery scan keeps its active, unfiltered preset duty; the connect
    // request carries the same duty plus the fixed link parameters.
    NimbleScaleClient c(false);
    testDiscParams={}; testConnectParams={};
    CHECK(c.startScan(nullptr,false,preset.first,preset.second,false));
    CHECK(testDiscParams.passive==0 && testDiscParams.filter_duplicates==0);
    CHECK(testDiscParams.filter_policy==0 && testDiscParams.limited==0);
    CHECK(testDiscParams.itvl==preset.first && testDiscParams.window==preset.second);
    advertise(c, BLE_HCI_ADV_RPT_EVTYPE_ADV_IND);
    c.service();
    CHECK(c.connectAttemptsTotal_==1);
    CHECK(testConnectParams.scan_itvl==preset.first && testConnectParams.scan_window==preset.second);
    CHECK(testConnectParams.itvl_min*125U==15U*100U && testConnectParams.itvl_max*125U==50U*100U);
    CHECK(testConnectParams.latency==0 && testConnectParams.supervision_timeout*10U==1000U);
    CHECK(testConnectParams.min_ce_len==0 && testConnectParams.max_ce_len==0);
  }
  {
    NimbleScaleClient c(false); ready(c);
    ble_gap_upd_params peer={6,3200,4,600,8,8}; ble_gap_upd_params self=peer;
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_CONN_UPDATE_REQ;
    e.conn_update_req.peer_params=&peer; e.conn_update_req.self_params=&self;
    e.conn_update_req.conn_handle=1;
    CHECK(c.onGapEvent(&e,c.linkOperationId_)==0); // Accepted with our parameters.
    CHECK(self.itvl_min==kConnIntervalMinUnits && self.itvl_max==kConnIntervalMaxUnits);
    CHECK(self.latency==0 && self.supervision_timeout==kConnSupervisionTimeoutUnits);
    CHECK(self.min_ce_len==8 && self.max_ce_len==8); // ce_len stays the host's.
    peer={16,24,0,100,0,0}; self=peer; e.type=BLE_GAP_EVENT_L2CAP_UPDATE_REQ;
    CHECK(c.onGapEvent(&e,c.linkOperationId_)==0);
    CHECK(self.itvl_min==16 && self.itvl_max==24 && self.latency==0);
    CHECK(self.supervision_timeout==kConnSupervisionTimeoutUnits);
    peer={48,8,0,100,0,0}; self=peer; // Inverted range collapses onto the cap.
    c.onGapEvent(&e,c.linkOperationId_);
    CHECK(self.itvl_min==kConnIntervalMaxUnits && self.itvl_max==kConnIntervalMaxUnits);
    CHECK(capturedScaleLogs.empty()); // Update requests log nothing on the host task.
    testConnDesc={1,24,0,kConnSupervisionTimeoutUnits}; testConnFindStatus=0;
    e.type=BLE_GAP_EVENT_CONN_UPDATE; e.conn_update.conn_handle=1;
    c.onGapEvent(&e,c.linkOperationId_+1); // Another link's update is not ours.
    c.service();
    CHECK(capturedScaleLogs.empty());
    c.onGapEvent(&e,c.linkOperationId_);
    CHECK(capturedScaleLogs.empty()); // Deferred to the worker.
    c.service();
    CHECK(capturedScaleLogs.size()==1 && capturedScaleLogTransport.back());
    CHECK(capturedScaleLogs.back().first==kScaleLogInfoSeverity);
    CHECK(capturedScaleLogs.back().second=="link params itvl_ms=30.00 latency=0 timeout_ms=1000");
    e.conn_update.status=BLE_HS_EUNKNOWN;
    c.onGapEvent(&e,c.linkOperationId_);
    c.service();
    CHECK(capturedScaleLogs.size()==1); // A failed update keeps the old parameters.
    testConnDesc={}; testConnFindStatus=BLE_HS_ENOTCONN;
  }
  {
    // A fresh connection reports its parameters once the worker services it.
    NimbleScaleClient c(false); ready(c);
    c.connectionHandle_=kInvalidHandle;
    c.enterState(NimbleScaleClient::State::Connecting);
    const uint32_t operation=c.beginOperation(NimbleScaleClient::CallbackDomain::Link);
    testConnDesc={7,12,0,kConnSupervisionTimeoutUnits}; testConnFindStatus=0;
    ble_gap_event e={}; e.type=BLE_GAP_EVENT_CONNECT; e.connect.conn_handle=7;
    c.onGapEvent(&e,operation);
    CHECK(capturedScaleLogs.empty());
    c.service();
    CHECK(!capturedScaleLogs.empty());
    CHECK(capturedScaleLogs.front().second=="link params itvl_ms=15.00 latency=0 timeout_ms=1000");
    testConnDesc={}; testConnFindStatus=BLE_HS_ENOTCONN;
    c.finishLink(false,ScaleDisconnectReason::HOST_RESET,0);
  }
  printf("NimBLE production client: %u checks passed\n",checks);
}
};
}
int main() { NimbleScaleClientTest::run(); }
