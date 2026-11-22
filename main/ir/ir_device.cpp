#include "ir_device.h"

#include "M5Unified.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <array>

namespace {
constexpr char kTag[] = "ir_remote";
constexpr gpio_num_t kReceivePin = GPIO_NUM_42;
constexpr gpio_num_t kSendPin = GPIO_NUM_46;
constexpr unsigned kResolutionHz = 1000000 / kIrTickUs;
constexpr int64_t kHighPowerSpacingUs = 500000;
int64_t s_next_high_power_us = 0;

// Only the IR screen owns these buffers. RMT uses them until the transaction completes.
std::array<rmt_symbol_word_t, (kIrMaxPulses + 3) / 2> s_rx_symbols{};
std::array<rmt_symbol_word_t, (kIrMaxPulses + 1) / 2> s_tx_symbols{};
IrSignal s_tx_signal;

bool IRAM_ATTR receive_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t *data, void *arg) {
    BaseType_t wake = pdFALSE;
    xQueueOverwriteFromISR(static_cast<QueueHandle_t>(arg), &data->num_symbols, &wake);
    return wake == pdTRUE;
}

bool IRAM_ATTR send_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *arg) {
    const size_t completed = 1;
    BaseType_t wake = pdFALSE;
    xQueueOverwriteFromISR(static_cast<QueueHandle_t>(arg), &completed, &wake);
    return wake == pdTRUE;
}

bool check(esp_err_t result, const char *step) {
    if (result == ESP_OK) return true;
    ESP_LOGW(kTag, "%s: %s", step, esp_err_to_name(result));
    return false;
}

bool apply_carrier(rmt_channel_handle_t tx, unsigned carrier_khz, bool high_power) {
    rmt_carrier_config_t carrier{};
    carrier.frequency_hz = carrier_khz * 1000;
    // The board fixes the LED peak current. Increase only the carrier's on-time.
    carrier.duty_cycle = high_power ? 0.50f : 0.33f;
    return check(rmt_apply_carrier(tx, &carrier), "TX carrier");
}
} // namespace

bool IrDevice::open() {
    if (M5.getBoard() != m5::board_t::board_M5StickS3) return false;
    events_ = xQueueCreate(1, sizeof(size_t));
    if (!events_) return false;

    rmt_rx_channel_config_t rx_config{};
    rx_config.gpio_num = kReceivePin;
    rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_config.mem_block_symbols = 96;
    rx_config.resolution_hz = kResolutionHz;
    rx_config.flags.invert_in = true; // The receiver pulls low when the IR carrier is present.
    if (!check(rmt_new_rx_channel(&rx_config, &rx_), "RX channel")) return false;
    rmt_rx_event_callbacks_t callbacks{};
    callbacks.on_recv_done = receive_done;
    if (!check(rmt_rx_register_event_callbacks(rx_, &callbacks, events_), "RX callback")) return false;

    rmt_tx_channel_config_t tx_config{};
    tx_config.gpio_num = kSendPin;
    tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_config.mem_block_symbols = 96;
    tx_config.resolution_hz = kResolutionHz;
    tx_config.trans_queue_depth = 1;
    if (!check(rmt_new_tx_channel(&tx_config, &tx_), "TX channel")) return false;

    rmt_tx_event_callbacks_t tx_callbacks{};
    tx_callbacks.on_trans_done = send_done;
    if (!check(rmt_tx_register_event_callbacks(tx_, &tx_callbacks, events_), "TX callback")) return false;

    if (!apply_carrier(tx_, carrier_khz_, false)) return false;
    rmt_copy_encoder_config_t copy{};
    if (!check(rmt_new_copy_encoder(&copy, &encoder_), "TX encoder")) return false;
    tx_enabled_ = check(rmt_enable(tx_), "TX enable");
    if (!tx_enabled_) return false;

    restore_power_ = !M5.Power.getExtOutput();
    if (restore_power_) {
        M5.Power.setExtOutput(true);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return true;
}

IrDevice::~IrDevice() {
    cancel_receive();
    if (tx_enabled_) {
        // A transmission is bounded to 1.2 s. Allow an in-flight send to finish before
        // releasing its encoder, even on an immediate exit by the user.
        rmt_tx_wait_all_done(tx_, 1500);
        rmt_disable(tx_);
    }
    if (tx_) rmt_del_channel(tx_);
    if (encoder_) rmt_del_encoder(encoder_);
    if (rx_) rmt_del_channel(rx_);
    if (events_) vQueueDelete(events_);
    if (restore_power_) M5.Power.setExtOutput(false);
}

bool IrDevice::receive(bool extended) {
    if (!rx_ || !events_) return false;
    if (!rx_enabled_) {
        rx_enabled_ = check(rmt_enable(rx_), "RX enable");
        if (!rx_enabled_) return false;
    }
    xQueueReset(events_);
    rmt_receive_config_t config{};
    config.signal_range_min_ns = 1250;
    config.signal_range_max_ns = (extended ? kIrLongGapUs : kIrSignalGapUs) * 1000;
    return check(rmt_receive(rx_, s_rx_symbols.data(), sizeof(s_rx_symbols), &config), "RX start");
}

IrReceive IrDevice::poll_receive(IrSignal &signal) {
    size_t count = 0;
    if (xQueueReceive(events_, &count, 0) != pdTRUE) return IrReceive::Pending;
    if (count == 0 || count >= s_rx_symbols.size()) return IrReceive::Rejected;

    signal.count = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto &symbol = s_rx_symbols[i];
        if (!ir_add_pulse(signal, symbol.level0, symbol.duration0 * kIrTickUs) ||
            !ir_add_pulse(signal, symbol.level1, symbol.duration1 * kIrTickUs)) return IrReceive::Rejected;
    }
    return ir_finish_signal(signal) ? IrReceive::Captured : IrReceive::Rejected;
}

void IrDevice::cancel_receive() {
    if (rx_enabled_) {
        rmt_disable(rx_);
        rx_enabled_ = false;
        xQueueReset(events_);
    }
}

bool IrDevice::send(const IrSignal &signal, const IrProfile &profile) {
    if (!tx_enabled_ || !ir_build_tx(signal, profile, s_tx_signal)) return false;
    cancel_receive();
    xQueueReset(events_);

    // Keep boosted bursts close to the LED on-time of a single remote-control frame.
    const int64_t now_us = esp_timer_get_time();
    const bool high = profile.high_power && now_us >= s_next_high_power_us && ir_supports_high_power(s_tx_signal);
    if (high != carrier_high_ || profile.carrier_khz != carrier_khz_) {
        if (!apply_carrier(tx_, profile.carrier_khz, high)) return false;
        carrier_high_ = high;
        carrier_khz_ = profile.carrier_khz;
    }

    const size_t count = (s_tx_signal.count + 1) / 2;
    for (size_t i = 0; i < count; ++i) {
        const size_t pulse = i * 2;
        rmt_symbol_word_t symbol{};
        symbol.level0 = 1;
        symbol.duration0 = (s_tx_signal.duration_us[pulse] + kIrTickUs / 2) / kIrTickUs;
        if (pulse + 1 < s_tx_signal.count) {
            symbol.duration1 = (s_tx_signal.duration_us[pulse + 1] + kIrTickUs / 2) / kIrTickUs;
        }
        s_tx_symbols[i] = symbol;
    }

    rmt_transmit_config_t config{};
    if (!check(rmt_transmit(tx_, encoder_, s_tx_symbols.data(),
                            count * sizeof(rmt_symbol_word_t), &config), "TX start")) return false;
    if (high) s_next_high_power_us = now_us + kHighPowerSpacingUs;
    return true;
}

bool IrDevice::sending_done() {
    size_t event;
    return xQueueReceive(events_, &event, 0) == pdTRUE;
}
