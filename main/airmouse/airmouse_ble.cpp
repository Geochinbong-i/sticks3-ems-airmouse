#include "airmouse_ble.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>

static constexpr const char *kTag = "sticks3_ble";
static std::atomic<BleStatus> s_status{BleStatus::Idle};
static std::atomic<bool> s_connected{false};
static std::atomic<bool> s_disconnecting{false};
static std::atomic<bool> s_want_adv{false};
static std::atomic<bool> s_time_sync_mode{false};
static std::atomic<bool> s_bonded_only{false};
static bool s_peer_was_bonded = false; // BLE callback task
static bool s_cts_opening = false;     // BLE callback task
static esp_gatt_if_t s_cts_link_if = ESP_GATT_IF_NONE;
static esp_bd_addr_t s_cts_peer{};
static esp_ble_addr_type_t s_cts_peer_type = BLE_ADDR_TYPE_PUBLIC;
static uint16_t s_cts_conn_id = 0;
static uint16_t s_cts_read_handle = 0;
using TimeWriteCallback = void (*)(const uint8_t *, uint16_t);
static std::atomic<TimeWriteCallback> s_time_write_callback{nullptr};
static uint16_t s_conn_id = 0;
static esp_bd_addr_t s_remote_bda{};

// Legacy advertising keeps the full name and 16-bit service UUID in the packet.
static uint8_t s_cts_adv_data[] = {
    2, ESP_BLE_AD_TYPE_FLAG, 0x06,
    // Time mode actually hosts both CTS and HID. iOS can pair with the HID accessory
    // from Settings, then expose its Current Time Service on the same encrypted link.
    5, ESP_BLE_AD_TYPE_16SRV_CMPL, 0x05, 0x18, 0x12, 0x18,
    3, ESP_BLE_AD_TYPE_APPEARANCE, 0xC2, 0x03,
    13, ESP_BLE_AD_TYPE_NAME_CMPL, 'S', 't', 'i', 'c', 'k', 'S', '3', '-', 'T', 'i', 'm', 'e',
};

static uint8_t s_mouse_adv_data[] = {
    2, ESP_BLE_AD_TYPE_FLAG, 0x06,
    3, ESP_BLE_AD_TYPE_16SRV_CMPL, 0x12, 0x18,
    3, ESP_BLE_AD_TYPE_APPEARANCE, 0xC2, 0x03,
    17, ESP_BLE_AD_TYPE_NAME_CMPL, 'S', 't', 'i', 'c', 'k', 'S', '3', ' ',
                                  'A', 'i', 'r', 'M', 'o', 'u', 's', 'e',
};
static_assert(sizeof(s_cts_adv_data) <= 31 && sizeof(s_mouse_adv_data) <= 31);

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0xA0, // 100 ms
    .adv_int_max = 0xF0, // 150 ms
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void ble_start_failed(const char *step, int status) {
    ESP_LOGW(kTag, "%s failed (%d)", step, status);
    if (s_want_adv) s_status = BleStatus::Failed;
}

static void start_advertising() {
    if (!s_want_adv || s_connected) return;
    s_status = BleStatus::Starting;
    const esp_err_t status = esp_ble_gap_start_advertising(&s_adv_params);
    if (status != ESP_OK) ble_start_failed("Request to advertise", status);
}

static void configure_advertising_profile() {
    const bool time_sync = s_time_sync_mode;
    esp_err_t status = esp_ble_gap_set_device_name(time_sync ? "StickS3-Time" : "StickS3 AirMouse");
    if (status == ESP_OK) {
        status = time_sync
            ? esp_ble_gap_config_adv_data_raw(s_cts_adv_data, sizeof(s_cts_adv_data))
            : esp_ble_gap_config_adv_data_raw(s_mouse_adv_data, sizeof(s_mouse_adv_data));
    }
    if (status != ESP_OK) ble_start_failed("Advertising configuration", status);
}

static bool is_bonded(const uint8_t *address) {
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;
    std::unique_ptr<esp_ble_bond_dev_t[]> peers(new (std::nothrow) esp_ble_bond_dev_t[count]);
    if (!peers || esp_ble_get_bond_device_list(&count, peers.get()) != ESP_OK) return false;
    for (int i = 0; i < count; ++i) {
        if (std::memcmp(peers[i].bd_addr, address, sizeof(esp_bd_addr_t)) == 0) return true;
    }
    return false;
}

static void receive_time(const uint8_t *data, uint16_t len) {
    if (!s_time_sync_mode || !s_want_adv || s_disconnecting ||
        (s_bonded_only && !s_peer_was_bonded)) return;
    if (auto cb = s_time_write_callback.load()) cb(data, len);
}

static void read_peer_cts(esp_gatt_if_t gattc_if, uint16_t conn_id) {
    if (!s_cts_opening || s_cts_read_handle != 0) return;

    esp_bt_uuid_t uuid{};
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = 0x1805;
    esp_gattc_service_elem_t service{};
    uint16_t count = 1;
    if (esp_ble_gattc_get_service(gattc_if, conn_id, &uuid, &service, &count, 0) != ESP_GATT_OK ||
        count == 0) return;

    uuid.uuid.uuid16 = 0x2A2B;
    esp_gattc_char_elem_t chr{};
    count = 1;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id, service.start_handle, service.end_handle,
                                      uuid, &chr, &count) != ESP_GATT_OK || count == 0) return;

    if (esp_ble_gattc_read_char(gattc_if, conn_id, chr.char_handle,
                               ESP_GATT_AUTH_REQ_NO_MITM) == ESP_OK) {
        s_cts_conn_id = conn_id;
        s_cts_read_handle = chr.char_handle;
    }
}

static void open_cts_client() {
    if (s_cts_opening || s_cts_link_if == ESP_GATT_IF_NONE) return;
    s_cts_opening = esp_ble_gattc_open(s_cts_link_if, s_cts_peer, s_cts_peer_type, true) == ESP_OK;
}

static void cts_client_event_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_DISCONNECT_EVT) {
        s_cts_link_if = ESP_GATT_IF_NONE;
        s_cts_opening = false;
        s_cts_read_handle = 0;
        return;
    }
    if (!s_time_sync_mode || !s_want_adv || s_disconnecting) return;

    switch (event) {
        case ESP_GATTC_CONNECT_EVT:
            // Reuse the central's physical link. A new peer must first finish pairing.
            s_cts_link_if = gattc_if;
            std::memcpy(s_cts_peer, param->connect.remote_bda, sizeof(s_cts_peer));
            s_cts_peer_type = param->connect.ble_addr_type;
            if (is_bonded(s_cts_peer)) open_cts_client();
            break;
        case ESP_GATTC_OPEN_EVT:
            // The peer may already be in the GATT cache after a previous connection.
            if (param->open.status == ESP_GATT_OK || param->open.status == ESP_GATT_ALREADY_OPEN) {
                read_peer_cts(gattc_if, param->open.conn_id);
            }
            break;
        case ESP_GATTC_DIS_SRVC_CMPL_EVT:
            if (param->dis_srvc_cmpl.status == ESP_GATT_OK) {
                read_peer_cts(gattc_if, param->dis_srvc_cmpl.conn_id);
            }
            break;
        case ESP_GATTC_READ_CHAR_EVT:
            if (s_cts_read_handle != 0 && param->read.status == ESP_GATT_OK &&
                param->read.conn_id == s_cts_conn_id &&
                param->read.handle == s_cts_read_handle && param->read.value_len == 10) {
                receive_time(param->read.value, param->read.value_len);
            }
            break;
        default:
            break;
    }
}

static void hidd_event_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch (event) {
        case ESP_HIDD_EVENT_SERVICES_READY:
            if (!s_want_adv) break;
            if (param->init_finish.state == ESP_HIDD_INIT_OK) {
                configure_advertising_profile();
            } else {
                ble_start_failed("GATT services", param->init_finish.state);
            }
            break;
        case ESP_HIDD_EVENT_BLE_CONNECT:
            s_conn_id = param->connect.conn_id;
            std::memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            s_peer_was_bonded = s_time_sync_mode && is_bonded(s_remote_bda);
            s_connected = true;
            if (!s_want_adv || (s_bonded_only && !s_peer_was_bonded)) {
                esp_ble_gap_disconnect(s_remote_bda);
            } else {
                s_status = BleStatus::Connected;
                esp_ble_set_encryption(s_remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
            }
            break;
        case ESP_HIDD_EVENT_BLE_DISCONNECT:
            s_peer_was_bonded = false;
            s_connected = false;
            s_disconnecting = false;
            start_advertising();
            break;
        default:
            break;
    }
}

static void gap_event_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (!s_want_adv) break;
            if (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                start_advertising();
            } else {
                ble_start_failed("Advertising packet", param->adv_data_raw_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (!s_want_adv || s_connected) break;
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                s_status = BleStatus::Advertising;
                ESP_LOGI(kTag, "Advertising %s", s_time_sync_mode ? "StickS3-Time" : "StickS3 AirMouse");
            } else {
                ble_start_failed("Advertising start", param->adv_start_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr,
                                     s_want_adv && (!s_bonded_only ||
                                         is_bonded(param->ble_security.ble_req.bd_addr)));
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            const auto &auth = param->ble_security.auth_cmpl;
            if (s_time_sync_mode && s_want_adv && !s_disconnecting && auth.success &&
                std::memcmp(auth.bd_addr, s_cts_peer, sizeof(s_cts_peer)) == 0 &&
                (!s_bonded_only || s_peer_was_bonded)) {
                open_cts_client();
            }
            break;
        }
        default:
            break;
    }
}

static esp_err_t start_ble_controller() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err == ESP_OK) err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err == ESP_OK) err = esp_bluedroid_init();
    if (err == ESP_OK) err = esp_bluedroid_enable();
    return err;
}

static esp_err_t register_ble_services() {
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    esp_err_t err = esp_ble_gap_register_callback(gap_event_cb);
    if (err != ESP_OK) return err;
    if (s_time_sync_mode) {
        err = esp_ble_gattc_register_callback(cts_client_event_cb);
        if (err == ESP_OK) err = esp_ble_gattc_app_register(0);
        if (err != ESP_OK) return err;
    }
    err = esp_hidd_profile_init();
    if (err != ESP_OK) return err;
    esp_hidd_set_time_write_cb(receive_time);
    return esp_hidd_register_callbacks(hidd_event_cb);
}

static bool start_ble(TimeWriteCallback on_time_write, bool bonded_only) {
    s_time_sync_mode = on_time_write != nullptr;
    s_time_write_callback = on_time_write;
    s_bonded_only = bonded_only;
    s_want_adv = true;
    s_status = BleStatus::Starting;

    esp_err_t err = start_ble_controller();
    if (err == ESP_OK && bonded_only && esp_ble_get_bond_device_num() <= 0) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) err = register_ble_services();
    if (err == ESP_OK) return true;

    ble_start_failed("BLE startup", err);
    airmouse_ble_stop();
    return false;
}

BleStatus airmouse_ble_status() {
    return s_status.load();
}

bool airmouse_ble_start() {
    return start_ble(nullptr, false);
}

void airmouse_ble_stop() {
    s_want_adv = false;
    s_time_write_callback = nullptr;

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        if (s_connected) {
            s_disconnecting = true;
            if (!s_time_sync_mode) esp_hidd_send_mouse_wheel(s_conn_id, 0, 0, 0, 0);
            if (esp_ble_gap_disconnect(s_remote_bda) == ESP_OK) {
                // Let the host receive the final mouse release or CTS write response.
                for (int i = 0; i < 20 && s_connected; ++i) vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            esp_ble_gap_stop_advertising();
        }
        esp_hidd_profile_deinit();
        const esp_err_t err = esp_bluedroid_disable();
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Bluetooth host disable: %s", esp_err_to_name(err));
            return;
        }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        const esp_err_t err = esp_bluedroid_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Bluetooth host teardown: %s", esp_err_to_name(err));
            return;
        }
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        const esp_err_t err = esp_bt_controller_disable();
        if (err != ESP_OK) ESP_LOGW(kTag, "Bluetooth controller disable: %s", esp_err_to_name(err));
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        const esp_err_t err = esp_bt_controller_deinit();
        if (err != ESP_OK) ESP_LOGW(kTag, "Bluetooth controller teardown: %s", esp_err_to_name(err));
    }

    s_peer_was_bonded = false;
    s_cts_link_if = ESP_GATT_IF_NONE;
    s_cts_opening = false;
    s_cts_read_handle = 0;
    s_connected = false;
    s_disconnecting = false;
    s_time_sync_mode = false;
    s_bonded_only = false;
    s_status = BleStatus::Idle;
}

bool airmouse_ble_start_time_sync(TimeWriteCallback on_time_write, bool bonded_only) {
    return start_ble(on_time_write, bonded_only);
}

void airmouse_ble_stop_time_sync() {
    airmouse_ble_stop();
}

bool airmouse_ble_is_connected() {
    return s_want_adv && s_connected && !s_disconnecting && !s_time_sync_mode;
}

void airmouse_ble_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!airmouse_ble_is_connected()) return;
    esp_hidd_send_mouse_wheel(s_conn_id, buttons, dx, dy, wheel);
}

void airmouse_ble_update_battery(int level) {
    if (!airmouse_ble_is_connected() || level < 0) return;
    if (level > 100) level = 100;
    esp_hidd_set_battery_level(s_conn_id, airmouse_ble_is_connected(), (uint8_t)level);
}
