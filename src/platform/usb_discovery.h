#pragma once

// Find the nth (0-indexed) USB serial device matching vidpid ("VID:PID").
// Returns a pointer to a static buffer, or NULL if not found.
const char *usb_find_serial_dev(const char *vidpid, int n);
