#pragma once

#include "ir_signal.h"

bool ir_load_slot(int slot, IrSignal &signal);
bool ir_save_slot(int slot, const IrSignal &signal);
