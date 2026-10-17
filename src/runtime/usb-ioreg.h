/*
 * IOKit registry property readers shared by the USB enumeration and usbdevfs
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * runtime/usb-sysfs.c models a device from these properties and
 * syscall/usbdev.c later matches the live service against that model, so both
 * must read a property the same way. Kept apart from usb-sysfs.h because that
 * header reaches path.c and fs.c, which must not include IOKit: Frama-C models
 * none of it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* Returns a new reference to property @key of @s, or NULL when it is absent or
 * the key cannot be built.
 */
static inline CFTypeRef usb_ioreg_prop(io_service_t s, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                              kCFStringEncodingUTF8);
    if (!k)
        return NULL;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    return v;
}

/* Returns the numeric property @key of @s, or -1 when it is absent or not a
 * number.
 */
static inline long usb_ioreg_num(io_service_t s, const char *key)
{
    CFTypeRef v = usb_ioreg_prop(s, key);
    long n = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef) v, kCFNumberLongType, &n);
    if (v)
        CFRelease(v);
    return n;
}

/* Copies the string property @key of @s into @out (@n bytes, always
 * terminated).
 *
 * Returns true when it is present, fits, and is non-empty.
 */
static inline bool usb_ioreg_str(io_service_t s,
                                 const char *key,
                                 char *out,
                                 size_t n)
{
    CFTypeRef v = usb_ioreg_prop(s, key);
    out[0] = '\0';
    bool ok = false;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = CFStringGetCString((CFStringRef) v, out, (CFIndex) n,
                                kCFStringEncodingUTF8) &&
             out[0] != '\0';
    if (v)
        CFRelease(v);
    return ok;
}
