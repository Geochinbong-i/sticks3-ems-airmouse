#include "airmouse_ble.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"

#include <cstring>

static bool s_bt_mem_released = false;
static bool s_ble_inited = false;
static bool s_connected = false;
static bool s_want_adv = false;
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_remote_bda{};

static constexpr uint16_t kMouseAppearance = 0x03C2; // HID Mouse

static uint8_t s_hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = kMouseAppearance,
    .manufacturer_len = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = sizeof(s_hidd_service_uuid128),
    .p_service_uuid = s_hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void hidd_event_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch (event) {
        case ESP_HIDD_EVENT_REG_FINISH:
            if (param->init_finish.state == ESP_HIDD_INIT_OK) {
                esp_ble_gap_set_device_name("StickS3 AirMouse");
                esp_ble_gap_config_adv_data(&s_adv_data);
            }
            break;
        case ESP_HIDD_EVENT_BLE_CONNECT:
            s_conn_id = param->connect.conn_id;
            std::memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            s_connected = true;
            break;
        case ESP_HIDD_EVENT_BLE_DISCONNECT:
            s_connected = false;
            if (s_want_adv) {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;
        default:
            break;
    }
}

static void gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            if (s_want_adv && !s_connected) {
                esp_ble_gap_start_advertising(&s_adv_params);
            }
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
        default:
            break;
    }
}

bool airmouse_ble_start() {
    s_want_adv = true;
    if (s_ble_inited) {
        if (!s_connected) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        return true;
    }

    if (!s_bt_mem_released) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        s_bt_mem_released = true;
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK) return false;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return false;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        if (esp_bluedroid_init() != ESP_OK) return false;
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        if (esp_bluedroid_enable() != ESP_OK) return false;
    }
    if (esp_hidd_profile_init() != ESP_OK) return false;

    esp_ble_gap_register_callback(gap_event_cb);
    esp_hidd_register_callbacks(hidd_event_cb);

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    s_ble_inited = true;
    return true;
}

void airmouse_ble_stop() {
    s_want_adv = false;
    if (!s_ble_inited) return;

    if (s_connected) {
        // Release any held mouse buttons before disconnecting
        esp_hidd_send_mouse_wheel(s_conn_id, 0, 0, 0, 0);
        esp_ble_gap_disconnect(s_remote_bda);
        s_connected = false;
    } else {
        esp_ble_gap_stop_advertising();
    }
}

bool airmouse_ble_is_connected() {
    return s_connected;
}

void airmouse_ble_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!s_connected) return;
    esp_hidd_send_mouse_wheel(s_conn_id, buttons, dx, dy, wheel);
}

void airmouse_ble_update_battery(int level) {
    if (!s_ble_inited || level < 0) return;
    if (level > 100) level = 100;
    esp_hidd_set_battery_level(s_conn_id, s_connected, (uint8_t)level);
}
