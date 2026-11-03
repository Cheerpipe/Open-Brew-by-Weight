#pragma once

// Deterministic platform boundary for the production client. Time advances
// only through waits; tests inject callbacks at those scheduling points.
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <new>
#include <cassert>
#include <mutex>
#include <memory>
#include <atomic>
#include <vector>

using TickType_t = uint32_t;
using TaskHandle_t = void *;
struct portMUX_TYPE {
  std::unique_ptr<std::recursive_mutex> mutex{new std::recursive_mutex};
};
#define portMUX_INITIALIZER_UNLOCKED {}
inline thread_local unsigned testCriticalDepth = 0;
inline thread_local std::function<void()> testAfterCriticalExit;
inline void testEnterCritical(portMUX_TYPE *mux) { mux->mutex->lock(); ++testCriticalDepth; }
inline void testExitCritical(portMUX_TYPE *mux) {
  assert(testCriticalDepth != 0);
  mux->mutex->unlock();
  if (--testCriticalDepth == 0 && testAfterCriticalExit) {
    auto callback = std::move(testAfterCriticalExit);
    testAfterCriticalExit = {};
    callback();
  }
}
#define portENTER_CRITICAL(m) testEnterCritical(m)
#define portEXIT_CRITICAL(m) testExitCritical(m)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
inline std::atomic<uint64_t> testNowMs{100};
inline uint32_t testWakeCount = 0;
inline std::function<void()> testOnWait;
inline std::function<void()> testOnDelay;
inline int64_t esp_timer_get_time() { return testNowMs * 1000; }
inline void vTaskDelay(uint32_t ms) {
  testNowMs += ms;
  if (testOnDelay) testOnDelay();
}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return &testWakeCount; }
inline void xTaskNotifyGive(TaskHandle_t) { ++testWakeCount; }
inline uint32_t ulTaskNotifyTake(int, uint32_t ms) {
  if (!testWakeCount && ms && testOnWait) testOnWait();
  const uint32_t count = testWakeCount;
  testWakeCount = 0;
  if (!count) testNowMs += ms;
  return count;
}
struct StaticSemaphore_t { bool signaled = false; };
using SemaphoreHandle_t = StaticSemaphore_t *;
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { return s; }
inline int xSemaphoreGive(SemaphoreHandle_t s) { s->signaled = true; return pdTRUE; }
inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ms) {
  if (!s->signaled && ms && testOnWait) testOnWait();
  if (s->signaled) { s->signaled = false; return pdTRUE; }
  testNowMs += ms;
  return pdFALSE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}

constexpr int BLE_HS_EAGAIN=1, BLE_HS_EALREADY=2, BLE_HS_EINVAL=3,
  BLE_HS_ENOENT=5, BLE_HS_ENOMEM=6, BLE_HS_ENOTCONN=7, BLE_HS_EAPP=9,
  BLE_HS_EBADDATA=10, BLE_HS_ETIMEOUT=13, BLE_HS_EDONE=14,
  BLE_HS_EBUSY=15, BLE_HS_EUNKNOWN=17, BLE_HS_ENOTSYNCED=22;
constexpr int BLE_HS_ERR_ATT_BASE=0x100, BLE_HS_ERR_HCI_BASE=0x200;
#define BLE_HS_HCI_ERR(x) (0x200+(x))
#define BLE_HS_ATT_ERR(x) (0x100+(x))
constexpr int BLE_HS_FOREVER=0x7fffffff;
constexpr uint8_t BLE_ERR_REM_USER_CONN_TERM=0x13;
constexpr int BLE_GAP_EVENT_DISC=1, BLE_GAP_EVENT_DISC_COMPLETE=2,
  BLE_GAP_EVENT_CONNECT=3, BLE_GAP_EVENT_DISCONNECT=4, BLE_GAP_EVENT_NOTIFY_RX=5,
  BLE_GAP_EVENT_CONN_UPDATE=6, BLE_GAP_EVENT_CONN_UPDATE_REQ=7,
  BLE_GAP_EVENT_L2CAP_UPDATE_REQ=8, BLE_GAP_EVENT_LINK_ESTAB=38;
constexpr int BLE_HCI_ADV_RPT_EVTYPE_ADV_IND=0, BLE_HCI_ADV_RPT_EVTYPE_DIR_IND=1,
  BLE_HCI_ADV_RPT_EVTYPE_ADV_NONCONN_IND=3, BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP=4;
constexpr uint8_t BLE_GATT_CHR_PROP_NOTIFY=0x10, BLE_GATT_CHR_PROP_INDICATE=0x20,
  BLE_GATT_CHR_PROP_WRITE=8, BLE_GATT_CHR_PROP_WRITE_NO_RSP=4;
constexpr uint16_t BLE_GATT_DSC_CLT_CFG_UUID16=0x2902;
struct ble_addr_t { uint8_t type; uint8_t val[6]; };
struct ble_uuid_t { uint16_t value; };
struct ble_uuid_any_t { ble_uuid_t u; };
inline int ble_uuid_from_str(ble_uuid_any_t *u, const char *s) {
  u->u.value = static_cast<uint16_t>(strtoul(s,nullptr,16)); return 0;
}
inline int ble_uuid_cmp(const ble_uuid_t *a,const ble_uuid_t *b) { return a->value != b->value; }
inline uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->value; }
struct os_mbuf { uint16_t length; uint8_t data[32]; };
inline uint16_t os_mbuf_len(const os_mbuf *b) { return b->length; }
inline int os_mbuf_copydata(os_mbuf *b,int offset,int length,void *out) {
  memcpy(out,b->data+offset,length); return 0;
}
struct ble_gap_disc_desc { ble_addr_t addr; uint8_t event_type; uint8_t *data; uint8_t length_data; };
struct ble_gap_upd_params { uint16_t itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len; };
struct ble_gap_conn_params { uint16_t scan_itvl,scan_window,itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len; };
struct ble_gap_conn_desc { uint16_t conn_handle,conn_itvl,conn_latency,supervision_timeout; };
struct ble_gap_event {
  int type;
  ble_gap_disc_desc disc;
  struct { int reason; } disc_complete;
  struct { int status; uint16_t conn_handle; } connect;
  struct { int status; uint16_t conn_handle; } link_estab;
  struct { int reason; struct { uint16_t conn_handle; } conn; } disconnect;
  struct { uint16_t conn_handle,attr_handle; os_mbuf *om; } notify_rx;
  struct { int status; uint16_t conn_handle; } conn_update;
  struct { const ble_gap_upd_params *peer_params; ble_gap_upd_params *self_params; uint16_t conn_handle; } conn_update_req;
};
// The stub keeps no connection table: tests publish the one descriptor a
// ble_gap_conn_find() may return.
inline ble_gap_conn_desc testConnDesc = {};
inline int testConnFindStatus = BLE_HS_ENOTCONN;
inline int ble_gap_conn_find(uint16_t, ble_gap_conn_desc *out) { *out = testConnDesc; return testConnFindStatus; }
struct ble_hs_adv_fields {
  uint8_t *name; uint8_t name_len,name_is_complete,num_uuids16;
  ble_uuid_any_t *uuids16;
};
inline ble_hs_adv_fields testAdvertisementFields = {};
inline int testAdvertisementParseStatus = BLE_HS_EINVAL;
inline int ble_hs_adv_parse_fields(ble_hs_adv_fields *fields, const uint8_t *, uint8_t) {
  *fields = testAdvertisementFields;
  return testAdvertisementParseStatus;
}
struct ble_gap_disc_params { uint8_t passive,filter_duplicates,filter_policy,limited; uint16_t itvl,window; };
struct ble_gatt_error { int status; uint16_t att_handle; };
struct ble_gatt_attr {};
struct ble_gatt_svc { uint16_t start_handle,end_handle; };
struct ble_gatt_chr { uint16_t def_handle,val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
using ble_gap_event_fn = int(ble_gap_event *, void *);
using TestWriteCallback = int(*)(uint16_t,const ble_gatt_error *,ble_gatt_attr *,void *);
using TestServiceCallback = int(*)(uint16_t,const ble_gatt_error *,const ble_gatt_svc *,void *);
using TestCharacteristicCallback = int(*)(uint16_t,const ble_gatt_error *,const ble_gatt_chr *,void *);
using TestDescriptorCallback = int(*)(uint16_t,const ble_gatt_error *,uint16_t,const ble_gatt_dsc *,void *);

// Every procedure a start call accepted, with the cb and cb_arg NimBLE would
// hold, until the test (or testHostReset) delivers its terminal callback.
enum class TestProcedureKind { Discovery, Connect, Services, Characteristics, Descriptors, Write };
struct TestProcedure {
  TestProcedureKind kind;
  void *arg;
  ble_gap_event_fn *gap;
  TestServiceCallback service;
  TestCharacteristicCallback characteristic;
  TestDescriptorCallback descriptor;
  TestWriteCallback write;
  uint16_t connHandle;  // GATT target, or the link once a connect succeeded.
  bool connected;
};
inline std::vector<TestProcedure> testProcedures;
inline TestProcedure testStarted(TestProcedureKind kind, void *arg, uint16_t connHandle) {
  TestProcedure procedure = {};
  procedure.kind = kind; procedure.arg = arg; procedure.connHandle = connHandle;
  return procedure;
}
inline bool testPending(TestProcedureKind kind) {
  for (const auto &procedure : testProcedures) if (procedure.kind == kind) return true;
  return false;
}
// Removes the newest pending procedure of this kind (or with this arg) the
// way NimBLE unlinks a procedure before its terminal callback.
inline TestProcedure testTake(TestProcedureKind kind, void *arg = nullptr) {
  for (size_t index = testProcedures.size(); index-- > 0;) {
    if (testProcedures[index].kind == kind && (arg == nullptr || testProcedures[index].arg == arg)) {
      const TestProcedure procedure = testProcedures[index];
      testProcedures.erase(testProcedures.begin() + static_cast<std::ptrdiff_t>(index));
      return procedure;
    }
  }
  assert(false && "no pending procedure");
  return {};
}
inline TestProcedure &testFind(TestProcedureKind kind) {
  for (size_t index = testProcedures.size(); index-- > 0;) {
    if (testProcedures[index].kind == kind) return testProcedures[index];
  }
  assert(false && "no pending procedure");
  return testProcedures.front();
}
inline void testGap(const TestProcedure &procedure, ble_gap_event event) {
  procedure.gap(&event, procedure.arg);
}
// ESP-IDF NimBLE reports every connect outcome as CONNECT then LINK_ESTAB.
inline void testConnectOutcome(const TestProcedure &procedure, int status, uint16_t connHandle) {
  ble_gap_event event = {};
  event.type = BLE_GAP_EVENT_CONNECT; event.connect.status = status; event.connect.conn_handle = connHandle;
  testGap(procedure, event);
  event.type = BLE_GAP_EVENT_LINK_ESTAB; event.link_estab.status = status; event.link_estab.conn_handle = connHandle;
  testGap(procedure, event);
}
inline void testGattTerminal(const TestProcedure &procedure, int status) {
  const ble_gatt_error error = {status, 0};
  switch (procedure.kind) {
    case TestProcedureKind::Services: procedure.service(procedure.connHandle, &error, nullptr, procedure.arg); break;
    case TestProcedureKind::Characteristics: procedure.characteristic(procedure.connHandle, &error, nullptr, procedure.arg); break;
    case TestProcedureKind::Descriptors: procedure.descriptor(procedure.connHandle, &error, 0, nullptr, procedure.arg); break;
    case TestProcedureKind::Write: procedure.write(procedure.connHandle, &error, nullptr, procedure.arg); break;
    default: assert(false && "not a GATT procedure");
  }
}
// ble_gap_conn_broken: GATT procedures of the link fail with ENOTCONN before
// the link's DISCONNECT.
inline void testDisconnect(uint16_t connHandle, int reason) {
  std::vector<TestProcedure> ended, kept;
  TestProcedure link = {};
  bool linkFound = false;
  for (const auto &procedure : testProcedures) {
    if (procedure.kind == TestProcedureKind::Connect && procedure.connected && procedure.connHandle == connHandle) {
      link = procedure; linkFound = true;
    } else if (procedure.kind >= TestProcedureKind::Services && procedure.connHandle == connHandle) {
      ended.push_back(procedure);
    } else {
      kept.push_back(procedure);
    }
  }
  testProcedures = kept;
  for (const auto &procedure : ended) testGattTerminal(procedure, BLE_HS_ENOTCONN);
  if (!linkFound) return;
  ble_gap_event event = {};
  event.type = BLE_GAP_EVENT_DISCONNECT; event.disconnect.reason = reason;
  event.disconnect.conn.conn_handle = connHandle;
  testGap(link, event);
}
// ble_hs reset: ble_gap_reset_state breaks every link, then
// ble_gap_master_failed ends the pending connect or discovery.
inline void testHostReset(int reason = BLE_HS_ENOTSYNCED) {
  std::vector<uint16_t> links;
  for (const auto &procedure : testProcedures) {
    if (procedure.kind == TestProcedureKind::Connect && procedure.connected) links.push_back(procedure.connHandle);
  }
  for (uint16_t link : links) testDisconnect(link, reason);
  while (!testProcedures.empty()) {
    const TestProcedure procedure = testProcedures.back();
    testProcedures.pop_back();
    if (procedure.kind == TestProcedureKind::Connect) {
      testConnectOutcome(procedure, reason, 0xffff);
    } else if (procedure.kind == TestProcedureKind::Discovery) {
      ble_gap_event event = {};
      event.type = BLE_GAP_EVENT_DISC_COMPLETE; event.disc_complete.reason = reason;
      testGap(procedure, event);
    } else {
      testGattTerminal(procedure, BLE_HS_ENOTCONN);
    }
  }
}

inline int testSubmitStatus=0, testTerminateStatus=0, testRssiStatus=0;
inline int testDiscStatus=0, testConnectStatus=BLE_HS_EINVAL, testGattDiscoveryStatus=BLE_HS_EINVAL;
inline unsigned testRadioProcedures=0, testTerminations=0, testWrites=0;
inline std::function<void()> testOnSubmit;
inline std::function<void()> testOnConnectCancel;
inline std::function<void()> testOnTerminate;
inline int testConnectCancelStatus=0;
inline int ble_gattc_write_flat(uint16_t connHandle,uint16_t,const void *,uint16_t,TestWriteCallback cb,void *arg) {
  ++testRadioProcedures; ++testWrites;
  if (testSubmitStatus == 0) {
    TestProcedure procedure = testStarted(TestProcedureKind::Write, arg, connHandle);
    procedure.write = cb;
    testProcedures.push_back(procedure);
  }
  if (testOnSubmit) testOnSubmit();
  return testSubmitStatus;
}
inline int ble_gattc_write_no_rsp_flat(uint16_t,uint16_t,const void *,uint16_t) { ++testRadioProcedures; ++testWrites; return testSubmitStatus; }
inline int ble_gattc_disc_all_svcs(uint16_t connHandle, TestServiceCallback cb, void *arg) {
  ++testRadioProcedures;
  if (testGattDiscoveryStatus != 0) return testGattDiscoveryStatus;
  TestProcedure procedure = testStarted(TestProcedureKind::Services, arg, connHandle);
  procedure.service = cb;
  testProcedures.push_back(procedure);
  return 0;
}
inline int ble_gattc_disc_all_chrs(uint16_t connHandle, uint16_t, uint16_t, TestCharacteristicCallback cb, void *arg) {
  ++testRadioProcedures;
  if (testGattDiscoveryStatus != 0) return testGattDiscoveryStatus;
  TestProcedure procedure = testStarted(TestProcedureKind::Characteristics, arg, connHandle);
  procedure.characteristic = cb;
  testProcedures.push_back(procedure);
  return 0;
}
inline int ble_gattc_disc_all_dscs(uint16_t connHandle, uint16_t, uint16_t, TestDescriptorCallback cb, void *arg) {
  ++testRadioProcedures;
  if (testGattDiscoveryStatus != 0) return testGattDiscoveryStatus;
  TestProcedure procedure = testStarted(TestProcedureKind::Descriptors, arg, connHandle);
  procedure.descriptor = cb;
  testProcedures.push_back(procedure);
  return 0;
}
inline ble_gap_conn_params testConnectParams = {};
inline ble_gap_disc_params testDiscParams = {};
inline int ble_gap_connect(uint8_t, const ble_addr_t *, int32_t, const ble_gap_conn_params *params, ble_gap_event_fn *cb, void *arg) {
  ++testRadioProcedures;
  testConnectParams = params == nullptr ? ble_gap_conn_params{} : *params;
  if (testConnectStatus != 0) return testConnectStatus;
  if (testPending(TestProcedureKind::Discovery)) return BLE_HS_EBUSY;
  TestProcedure procedure = testStarted(TestProcedureKind::Connect, arg, 0xffff);
  procedure.gap = cb;
  testProcedures.push_back(procedure);
  return 0;
}
inline int ble_gap_disc(uint8_t, int32_t, const ble_gap_disc_params *params, ble_gap_event_fn *cb, void *arg) {
  ++testRadioProcedures;
  testDiscParams = params == nullptr ? ble_gap_disc_params{} : *params;
  if (testDiscStatus != 0) return testDiscStatus;
  if (testPending(TestProcedureKind::Discovery)) return BLE_HS_EALREADY;
  TestProcedure procedure = testStarted(TestProcedureKind::Discovery, arg, 0xffff);
  procedure.gap = cb;
  testProcedures.push_back(procedure);
  return 0;
}
// A successful cancel ends the discovery without DISC_COMPLETE.
inline int ble_gap_disc_cancel() {
  ++testRadioProcedures;
  if (!testPending(TestProcedureKind::Discovery)) return BLE_HS_EALREADY;
  (void)testTake(TestProcedureKind::Discovery);
  return 0;
}
inline bool testDiscoveryActive = true;
inline int ble_gap_disc_active() { return testDiscoveryActive; }
inline int ble_gap_conn_cancel() {
  ++testRadioProcedures;
  if (testOnConnectCancel) testOnConnectCancel();
  return testConnectCancelStatus;
}
inline int ble_gap_terminate(uint16_t,uint8_t) {
  ++testRadioProcedures; ++testTerminations;
  if (testOnTerminate) testOnTerminate();
  return testTerminateStatus;
}
inline int ble_gap_conn_rssi(uint16_t,int8_t *rssi) { ++testRadioProcedures; *rssi=-50; return testRssiStatus; }
struct OpenBrewByWeightBleHealth { int lastResetReason; };
inline bool testRuntimeReady=true;
inline uint32_t testSyncGeneration=1;
inline bool openBrewByWeightBleRuntimeReady() { return testRuntimeReady; }
inline uint32_t openBrewByWeightBleRuntimeSyncGeneration() { return testSyncGeneration; }
inline uint8_t openBrewByWeightBleRuntimeOwnAddressType() { return 0; }
inline OpenBrewByWeightBleHealth openBrewByWeightBleRuntimeHealth() { return {BLE_HS_ENOTSYNCED}; }
