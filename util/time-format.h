/*
 * Time formatting utilities for QEMU
 */

#ifndef QEMU_TIME_FORMAT_H
#define QEMU_TIME_FORMAT_H

#include "qemu/osdep.h"

/**
 * format_time_delta: Format nanoseconds in human-readable time units
 *
 * @ns: nanoseconds (can be negative, will be shown as absolute value)
 * @buf: output buffer
 * @bufsize: size of output buffer
 * @returns: pointer to buf for convenience
 */
const char *format_time_delta(int64_t ns, char *buf, size_t bufsize);

#endif /* QEMU_TIME_FORMAT_H */
