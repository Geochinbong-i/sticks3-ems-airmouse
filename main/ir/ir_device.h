#pragma once

#include "ir_signal.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum class IrReceive { Pending, Captured, Rejected };

class IrDevice {
public:
    IrDevice() = default;
    ~IrDevice();
    IrDevice(const IrDevice &) = delete;
    IrDevice &operator=(const IrDevice &) = delete;

    bool open();
    bool receive(bool extended);
    IrReceive poll_receive(IrSignal &signal);
    void cancel_receive();
    bool send(const IrSignal &signal, const IrProfile &profile);
    bool sending_done();
    bool sent_at_high_power() const { return carrier_high_; }

private:
    QueueHandle_t events_ = nullptr;
    rmt_channel_handle_t rx_ = nullptr;
    rmt_channel_handle_t tx_ = nullptr;
    rmt_encoder_handle_t encoder_ = nullptr;
    bool rx_enabled_ = false;
    bool tx_enabled_ = false;
    bool restore_power_ = false;
    unsigned carrier_khz_ = 38;
    bool carrier_high_ = false;
};
