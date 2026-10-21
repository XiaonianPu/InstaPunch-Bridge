#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef uint8_t esp_bd_addr_t[6];
typedef uint8_t esp_bt_pin_code_t[16];
typedef int esp_bt_io_cap_t;
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_NO_MEM -3
#define ESP_BD_ADDR_LEN 6
#define ESP_BD_ADDR_STR "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(a) (a)[0],(a)[1],(a)[2],(a)[3],(a)[4],(a)[5]
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define ESP_ERROR_CHECK(x) ((void)(x))
#define ESP_SPP_SUCCESS 0
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_BT_INQ_MODE_GENERAL_INQUIRY 0
#define ESP_BT_DISCOVERY_STOPPED 0
#define ESP_BT_GAP_DISCOVERY_STOPPED 0
#define ESP_BT_COD_MAJOR_DEV_UNCATEGORIZED 31
#define ESP_BT_NON_CONNECTABLE 0
#define ESP_BT_NON_DISCOVERABLE 0
#define ESP_SPP_SEC_AUTHENTICATE 0
#define ESP_SPP_ROLE_MASTER 0
#define ESP_SPP_MODE_CB 0
#define ESP_BT_IO_CAP_NONE 0
#define ESP_BT_SP_IOCAP_MODE 0
#define ESP_BT_PIN_TYPE_VARIABLE 0
#define ESP_BT_EIR_TYPE_CMPL_16BITS_UUID 1
#define ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID 2
#define ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME 3
#define ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME 4
enum { ESP_BT_GAP_DEV_PROP_RSSI, ESP_BT_GAP_DEV_PROP_COD, ESP_BT_GAP_DEV_PROP_BDNAME, ESP_BT_GAP_DEV_PROP_EIR };
typedef enum { ESP_BT_GAP_DISC_RES_EVT, ESP_BT_GAP_DISC_STATE_CHANGED_EVT, ESP_BT_GAP_READ_REMOTE_NAME_EVT, ESP_BT_GAP_AUTH_CMPL_EVT, ESP_BT_GAP_PIN_REQ_EVT, ESP_BT_GAP_CFM_REQ_EVT, ESP_BT_GAP_READ_RSSI_DELTA_EVT } esp_bt_gap_cb_event_t;
typedef struct { int type, len; void *val; } esp_bt_gap_dev_prop_t;
typedef union {
 struct { uint8_t bda[6]; int num_prop; esp_bt_gap_dev_prop_t *prop; } disc_res;
 struct { int state; } disc_st_chg;
 struct { uint8_t bda[6]; int stat; uint8_t rmt_name[249]; } read_rmt_name;
 struct { uint8_t bda[6]; int stat; char device_name[249]; } auth_cmpl;
 struct { uint8_t bda[6]; bool min_16_digit; } pin_req;
 struct { uint8_t bda[6]; } cfm_req;
 struct { uint8_t bda[6]; int stat; int8_t rssi_delta; } read_rssi_delta;
} esp_bt_gap_cb_param_t;
typedef enum { ESP_SPP_INIT_EVT, ESP_SPP_DISCOVERY_COMP_EVT, ESP_SPP_OPEN_EVT, ESP_SPP_CLOSE_EVT, ESP_SPP_WRITE_EVT, ESP_SPP_CONG_EVT, ESP_SPP_DATA_IND_EVT, ESP_SPP_CL_INIT_EVT } esp_spp_cb_event_t;
typedef union {
 struct { int status; uint8_t scn_num, scn[32]; } disc_comp;
 struct { int status; uint32_t handle; uint8_t rem_bda[6]; } open;
 struct { int status; uint32_t handle; } cl_init;
 struct { int status; uint32_t port_status, handle; bool async; } close;
 struct { int status; uint32_t handle; bool cong; } write;
 struct { uint32_t handle; bool cong; } cong;
 struct { uint32_t handle; uint8_t *data; size_t len; } data_ind;
} esp_spp_cb_param_t;
typedef struct { int mode; bool enable_l2cap_ertm; int tx_buffer_size; } esp_spp_cfg_t;
extern TickType_t mock_ticks;
extern int mock_discovers, mock_connects, mock_disconnects, mock_scans, mock_rssi_reads;
extern esp_err_t mock_connect_result;
static inline void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI mock_log
#define ESP_LOGW mock_log
#define ESP_LOGE mock_log
static inline const char *esp_err_to_name(int e) { return "mock"; }
static inline TickType_t xTaskGetTickCount(void) { return mock_ticks; }
static inline int64_t esp_timer_get_time(void) { return (int64_t)mock_ticks * 1000; }
static inline void vTaskDelay(TickType_t t) {}
static inline int xTaskCreate(void (*f)(void *), const char *n, int stack, void *arg, int pri, void *out) { return 1; }
static inline void *xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
static inline void xSemaphoreTakeRecursive(void *s, uint32_t t) {}
static inline void xSemaphoreGiveRecursive(void *s) {}
static inline int esp_spp_disconnect(uint32_t h) { mock_disconnects++; return ESP_OK; }
static inline int esp_spp_connect(int sec, int role, uint8_t scn, uint8_t *mac) { mock_connects++; return mock_connect_result; }
static inline int esp_spp_start_discovery(uint8_t *mac) { mock_discovers++; return ESP_OK; }
static inline int esp_bt_gap_start_discovery(int m,int l,int n) { mock_scans++; return ESP_OK; }
static inline int esp_bt_gap_read_remote_name(uint8_t *mac) { return ESP_OK; }
static inline int esp_bt_gap_read_rssi_delta(uint8_t *mac) { mock_rssi_reads++; return ESP_OK; }
static inline uint32_t esp_bt_gap_get_cod_major_dev(uint32_t cod) { return (cod >> 8) & 31; }
static inline uint8_t *esp_bt_gap_resolve_eir_data(uint8_t *e,int type,uint8_t *len) { return NULL; }
static inline int esp_bt_gap_pin_reply(uint8_t *m,bool a,int len,uint8_t *p) { return ESP_OK; }
static inline int esp_bt_gap_ssp_confirm_reply(uint8_t *m,bool a) { return ESP_OK; }
static inline int esp_bt_gap_set_scan_mode(int a,int b) { return ESP_OK; }
static inline int esp_bt_gap_register_callback(void (*cb)(esp_bt_gap_cb_event_t,esp_bt_gap_cb_param_t *)) { return ESP_OK; }
static inline int esp_bt_gap_set_security_param(int a,void *b,int c) { return ESP_OK; }
static inline int esp_bt_gap_set_pin(int a,int b,uint8_t *c) { return ESP_OK; }
static inline int esp_spp_register_callback(void (*cb)(esp_spp_cb_event_t,esp_spp_cb_param_t *)) { return ESP_OK; }
static inline int esp_spp_enhanced_init(const esp_spp_cfg_t *c) { return ESP_OK; }
static inline int esp_bt_gap_remove_bond_device(uint8_t *m) { return ESP_OK; }
static inline int esp_spp_write(uint32_t h,int len,uint8_t *data) { return ESP_OK; }
static inline int esp_bt_gap_get_bond_device_num(void) { return 0; }
static inline int esp_bt_gap_get_bond_device_list(int *n,esp_bd_addr_t *list) { return ESP_OK; }
