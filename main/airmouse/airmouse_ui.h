#pragma once

enum class AirMouseTransport {
    Ble = 0,
    Usb = 1,
};

void airmouse_ui_run(AirMouseTransport transport);
