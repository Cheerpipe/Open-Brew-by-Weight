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
  BLE_HS_ENOENT=5, BLE_HS_ENOMEM=6, BLE_HS_ENOTCONN=7,
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
  BLE_GAP_EVENT_L2CAP_UPDATE_REQ=8;
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
#define OS_MBUF_PKTLEN(b) ((b)->length)
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
using TestWriteCallback = int(*)(uint16_t,const ble_gatt_error *,ble_gatt_attr *,void *);
inline TestWriteCallback testWriteCallback=nullptr;
inline void *testWriteArg=nullptr;
inline int testSubmitStatus=0, testTerminateStatus=0, testRssiStatus=0;
inline unsigned testRadioProcedures=0, testTerminations=0, testWrites=0;
inline std::function<void()> testOnSubmit;
inline std::function<void()> testOnConnectCancel;
inline std::function<void()> testOnTerminate;
inline int testConnectCancelStatus=0;
inline int ble_gattc_write_flat(uint16_t,uint16_t,const void *,uint16_t,TestWriteCallback cb,void *arg) {
  ++testRadioProcedures; ++testWrites;
  testWriteCallback=cb; testWriteArg=arg;
  if (testOnSubmit) testOnSubmit();
  return testSubmitStatus;
}
inline int ble_gattc_write_no_rsp_flat(uint16_t,uint16_t,const void *,uint16_t) { ++testRadioProcedures; ++testWrites; return testSubmitStatus; }
template<class... T> int ble_gattc_disc_all_svcs(T...) { ++testRadioProcedures; return BLE_HS_EINVAL; }
template<class... T> int ble_gattc_disc_all_chrs(T...) { ++testRadioProcedures; return BLE_HS_EINVAL; }
template<class... T> int ble_gattc_disc_all_dscs(T...) { ++testRadioProcedures; return BLE_HS_EINVAL; }
using ble_gap_event_fn = int(ble_gap_event *, void *);
inline ble_gap_conn_params testConnectParams = {};
inline ble_gap_disc_params testDiscParams = {};
inline int ble_gap_connect(uint8_t, const ble_addr_t *, int32_t, const ble_gap_conn_params *params, ble_gap_event_fn *, void *) {
  ++testRadioProcedures;
  testConnectParams = params == nullptr ? ble_gap_conn_params{} : *params;
  return BLE_HS_EINVAL;
}
inline int ble_gap_disc(uint8_t, int32_t, const ble_gap_disc_params *params, ble_gap_event_fn *, void *) {
  ++testRadioProcedures;
  testDiscParams = params == nullptr ? ble_gap_disc_params{} : *params;
  return 0;
}
inline int ble_gap_disc_cancel() { ++testRadioProcedures; return 0; }
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
