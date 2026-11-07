#include "pico/stdlib.h"

// On this RP2350 A2, the normal semaphore-based response wait can stall
// without driver logging. Only replace the short IOCTL/bus response wait;
// keep the SDK itself intact and allow background IRQ processing throughout.
// The CYW43 driver's own IOCTL deadline still bounds the entire operation.
void __wrap_cyw43_await_background_or_timeout_us(uint32_t timeout_us) {
    busy_wait_us_32(timeout_us);
}
