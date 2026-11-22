#pragma once

#include "ir_signal.h"

// For legacy slots the caller's default profile is retained.
bool ir_load_slot(int slot, IrSignal &signal, IrProfile &profile);
bool ir_save_slot(int slot, const IrSignal &signal, const IrProfile &profile);
