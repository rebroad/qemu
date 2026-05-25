/*
 * Time formatting utilities for QEMU
 */

#include "qemu/osdep.h"
#include "util/time-format.h"

/**
 * format_time_delta: Format nanoseconds in human-readable time units
 *
 * @ns: nanoseconds (can be negative, will be shown as absolute value)
 * @buf: output buffer
 * @bufsize: size of output buffer
 * @returns: pointer to buf for convenience
 */
const char *format_time_delta(int64_t ns, char *buf, size_t bufsize)
{
    int64_t abs_ns = ns < 0 ? -ns : ns;
    
    if (abs_ns >= 3600LL * 1000000000LL) {
        // Hours (if >= 1 hour)
        double hours = (double)abs_ns / (3600.0 * 1000000000.0);
        snprintf(buf, bufsize, "%.1fh", hours);
    } else if (abs_ns >= 60LL * 1000000000LL) {
        // Minutes (if >= 1 minute)
        double minutes = (double)abs_ns / (60.0 * 1000000000.0);
        snprintf(buf, bufsize, "%.1fm", minutes);
    } else if (abs_ns >= 1000000000LL) {
        // Seconds (if >= 1 second)
        double seconds = (double)abs_ns / 1000000000.0;
        snprintf(buf, bufsize, "%.1fs", seconds);
    } else if (abs_ns >= 1000000LL) {
        // Milliseconds (if >= 1ms)
        double ms = (double)abs_ns / 1000000.0;
        snprintf(buf, bufsize, "%.1fms", ms);
    } else if (abs_ns >= 1000LL) {
        // Microseconds (if >= 1µs)
        double us = (double)abs_ns / 1000.0;
        snprintf(buf, bufsize, "%.1fµs", us);
    } else {
        // Nanoseconds
        snprintf(buf, bufsize, "%ldns", (long)abs_ns);
    }
    
    return buf;
}
