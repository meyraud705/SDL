/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "../SDL_sysjoystick.h"
#include "SDL_hidapijoystick_c.h"
#include "SDL_hidapi_rumble.h"
#include "../../SDL_hints_c.h"

#if defined(__WIN32__) || defined(__WINGDK__)
#include "../windows/SDL_rawinputjoystick_c.h"
#endif

#ifdef SDL_USE_LIBUDEV
#include "../../core/linux/SDL_sandbox.h"
#endif

struct joystick_hwdata
{
    SDL_HIDAPI_Device *device;
};

static SDL_HIDAPI_DeviceDriver *SDL_HIDAPI_drivers[] = {
#ifdef SDL_JOYSTICK_HIDAPI_GAMECUBE
    &SDL_HIDAPI_DriverGameCube,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_LUNA
    &SDL_HIDAPI_DriverLuna,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_SHIELD
    &SDL_HIDAPI_DriverShield,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_PS3
    &SDL_HIDAPI_DriverPS3,
    &SDL_HIDAPI_DriverPS3ThirdParty,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_PS4
    &SDL_HIDAPI_DriverPS4,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_PS5
    &SDL_HIDAPI_DriverPS5,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_STADIA
    &SDL_HIDAPI_DriverStadia,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_STEAM
    &SDL_HIDAPI_DriverSteam,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_SWITCH
    &SDL_HIDAPI_DriverNintendoClassic,
    &SDL_HIDAPI_DriverJoyCons,
    &SDL_HIDAPI_DriverSwitch,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_WII
    &SDL_HIDAPI_DriverWii,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_XBOX360
    &SDL_HIDAPI_DriverXbox360,
    &SDL_HIDAPI_DriverXbox360W,
#endif
#ifdef SDL_JOYSTICK_HIDAPI_XBOXONE
    &SDL_HIDAPI_DriverXboxOne,
#endif
};
static int SDL_HIDAPI_numdrivers = 0;
static SDL_SpinLock SDL_HIDAPI_spinlock;
static SDL_bool SDL_HIDAPI_hints_changed = SDL_FALSE;
static Uint32 SDL_HIDAPI_change_count = 0;
static SDL_HIDAPI_Device *SDL_HIDAPI_devices SDL_GUARDED_BY(SDL_joystick_lock);
static char SDL_HIDAPI_device_magic;
static int SDL_HIDAPI_numjoysticks = 0;
static SDL_bool SDL_HIDAPI_combine_joycons = SDL_TRUE;
static SDL_bool initialized = SDL_FALSE;
static SDL_bool shutting_down = SDL_FALSE;

static char *HIDAPI_ConvertString(const wchar_t *wide_string)
{
    char *string = NULL;

    if (wide_string) {
        string = SDL_iconv_string("UTF-8", "WCHAR_T", (char *)wide_string, (SDL_wcslen(wide_string) + 1) * sizeof(wchar_t));
        if (string == NULL) {
            switch (sizeof(wchar_t)) {
            case 2:
                string = SDL_iconv_string("UTF-8", "UCS-2-INTERNAL", (char *)wide_string, (SDL_wcslen(wide_string) + 1) * sizeof(wchar_t));
                break;
            case 4:
                string = SDL_iconv_string("UTF-8", "UCS-4-INTERNAL", (char *)wide_string, (SDL_wcslen(wide_string) + 1) * sizeof(wchar_t));
                break;
            }
        }
    }
    return string;
}

void HIDAPI_DumpPacket(const char *prefix, const Uint8 *data, int size)
{
    int i;
    char *buffer;
    size_t length = SDL_strlen(prefix) + 11 * (USB_PACKET_LENGTH / 8) + (5 * USB_PACKET_LENGTH * 2) + 1 + 1;
    int start = 0, amount = size;
    size_t current_len;

    buffer = (char *)SDL_malloc(length);
    current_len = SDL_snprintf(buffer, length, prefix, size);
    for (i = start; i < start + amount; ++i) {
        if ((i % 8) == 0) {
            current_len += SDL_snprintf(&buffer[current_len], length - current_len, "\n%.2d:      ", i);
        }
        current_len += SDL_snprintf(&buffer[current_len], length - current_len, " 0x%.2x", data[i]);
    }
    SDL_strlcat(buffer, "\n", length);
    SDL_Log("%s", buffer);
    SDL_free(buffer);
}

SDL_bool HIDAPI_SupportsPlaystationDetection(Uint16 vendor, Uint16 product)
{
    /* If we already know the controller is a different type, don't try to detect it.
     * This fixes a hang with the HORIPAD for Nintendo Switch (0x0f0d/0x00c1)
     */
    if (SDL_GetGamepadTypeFromVIDPID(vendor, product, NULL, SDL_FALSE) != SDL_GAMEPAD_TYPE_UNKNOWN) {
        return SDL_FALSE;
    }

    switch (vendor) {
    case USB_VENDOR_DRAGONRISE:
        return SDL_TRUE;
    case USB_VENDOR_HORI:
        return SDL_TRUE;
    case USB_VENDOR_LOGITECH:
        /* Most Logitech devices are fine with this, but the F310 will lock up */
        if (product == USB_PRODUCT_LOGITECH_F310) {
            return SDL_FALSE;
        }
        return SDL_TRUE;
    case USB_VENDOR_MADCATZ:
        return SDL_TRUE;
    case USB_VENDOR_NACON:
        return SDL_TRUE;
    case USB_VENDOR_PDP:
        return SDL_TRUE;
    case USB_VENDOR_POWERA:
        return SDL_TRUE;
    case USB_VENDOR_POWERA_ALT:
        return SDL_TRUE;
    case USB_VENDOR_QANBA:
        return SDL_TRUE;
    case USB_VENDOR_RAZER:
        /* Most Razer devices are not game controllers, and some of them lock up
         * or reset when we send them the Sony third-party query feature report,
         * so don't include that vendor here. Instead add devices as appropriate
         * to controller_type.c
         *
         * Reference: https://github.com/libsdl-org/SDL/issues/6733
         *            https://github.com/libsdl-org/SDL/issues/6799
         */
        return SDL_FALSE;
    case USB_VENDOR_SHANWAN:
        return SDL_TRUE;
    case USB_VENDOR_SHANWAN_ALT:
        return SDL_TRUE;
    case USB_VENDOR_THRUSTMASTER:
        return SDL_TRUE;
    case USB_VENDOR_ZEROPLUS:
        return SDL_TRUE;
    case 0x7545 /* SZ-MYPOWER */:
        return SDL_TRUE;
    default:
        return SDL_FALSE;
    }
}

float HIDAPI_RemapVal(float val, float val_min, float val_max, float output_min, float output_max)
{
    return output_min + (output_max - output_min) * (val - val_min) / (val_max - val_min);
}

static void HIDAPI_UpdateDeviceList(void);
static void HIDAPI_JoystickClose(SDL_Joystick *joystick);

static SDL_GamepadType SDL_GetJoystickGameControllerProtocol(const char *name, Uint16 vendor, Uint16 product, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    static const int LIBUSB_CLASS_VENDOR_SPEC = 0xFF;
    static const int XB360_IFACE_SUBCLASS = 93;
    static const int XB360_IFACE_PROTOCOL = 1;    /* Wired */
    static const int XB360W_IFACE_PROTOCOL = 129; /* Wireless */
    static const int XBONE_IFACE_SUBCLASS = 71;
    static const int XBONE_IFACE_PROTOCOL = 208;

    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;

    /* This code should match the checks in libusb/hid.c and HIDDeviceManager.java */
    if (interface_class == LIBUSB_CLASS_VENDOR_SPEC &&
        interface_subclass == XB360_IFACE_SUBCLASS &&
        (interface_protocol == XB360_IFACE_PROTOCOL ||
         interface_protocol == XB360W_IFACE_PROTOCOL)) {

        static const int SUPPORTED_VENDORS[] = {
            0x0079, /* GPD Win 2 */
            0x044f, /* Thrustmaster */
            0x045e, /* Microsoft */
            0x046d, /* Logitech */
            0x056e, /* Elecom */
            0x06a3, /* Saitek */
            0x0738, /* Mad Catz */
            0x07ff, /* Mad Catz */
            0x0e6f, /* PDP */
            0x0f0d, /* Hori */
            0x1038, /* SteelSeries */
            0x11c9, /* Nacon */
            0x12ab, /* Unknown */
            0x1430, /* RedOctane */
            0x146b, /* BigBen */
            0x1532, /* Razer */
            0x15e4, /* Numark */
            0x162e, /* Joytech */
            0x1689, /* Razer Onza */
            0x1949, /* Lab126, Inc. */
            0x1bad, /* Harmonix */
            0x20d6, /* PowerA */
            0x24c6, /* PowerA */
            0x2c22, /* Qanba */
            0x2dc8, /* 8BitDo */
            0x9886, /* ASTRO Gaming */
        };

        int i;
        for (i = 0; i < SDL_arraysize(SUPPORTED_VENDORS); ++i) {
            if (vendor == SUPPORTED_VENDORS[i]) {
                type = SDL_GAMEPAD_TYPE_XBOX360;
                break;
            }
        }
    }

    if (interface_number == 0 &&
        interface_class == LIBUSB_CLASS_VENDOR_SPEC &&
        interface_subclass == XBONE_IFACE_SUBCLASS &&
        interface_protocol == XBONE_IFACE_PROTOCOL) {

        static const int SUPPORTED_VENDORS[] = {
            0x044f, /* Thrustmaster */
            0x045e, /* Microsoft */
            0x0738, /* Mad Catz */
            0x0e6f, /* PDP */
            0x0f0d, /* Hori */
            0x10f5, /* Turtle Beach */
            0x1532, /* Razer */
            0x20d6, /* PowerA */
            0x24c6, /* PowerA */
            0x2dc8, /* 8BitDo */
            0x2e24, /* Hyperkin */
            0x3537, /* GameSir */
        };

        int i;
        for (i = 0; i < SDL_arraysize(SUPPORTED_VENDORS); ++i) {
            if (vendor == SUPPORTED_VENDORS[i]) {
                type = SDL_GAMEPAD_TYPE_XBOXONE;
                break;
            }
        }
    }

    if (type == SDL_GAMEPAD_TYPE_UNKNOWN) {
        type = SDL_GetGamepadTypeFromVIDPID(vendor, product, name, SDL_FALSE);
    }
    return type;
}

static SDL_bool HIDAPI_IsDeviceSupported(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    int i;
    SDL_GamepadType type = SDL_GetJoystickGameControllerProtocol(name, vendor_id, product_id, -1, 0, 0, 0);

    for (i = 0; i < SDL_arraysize(SDL_HIDAPI_drivers); ++i) {
        SDL_HIDAPI_DeviceDriver *driver = SDL_HIDAPI_drivers[i];
        if (driver->enabled && driver->IsSupportedDevice(NULL, name, type, vendor_id, product_id, version, -1, 0, 0, 0)) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

static SDL_HIDAPI_DeviceDriver *HIDAPI_GetDeviceDriver(SDL_HIDAPI_Device *device)
{
    const Uint16 USAGE_PAGE_GENERIC_DESKTOP = 0x0001;
    const Uint16 USAGE_JOYSTICK = 0x0004;
    const Uint16 USAGE_GAMEPAD = 0x0005;
    const Uint16 USAGE_MULTIAXISCONTROLLER = 0x0008;
    int i;

    if (device->num_children > 0) {
        return &SDL_HIDAPI_DriverCombined;
    }

    if (SDL_ShouldIgnoreJoystick(device->name, device->guid)) {
        return NULL;
    }

    if (device->vendor_id != USB_VENDOR_VALVE) {
        if (device->usage_page && device->usage_page != USAGE_PAGE_GENERIC_DESKTOP) {
            return NULL;
        }
        if (device->usage && device->usage != USAGE_JOYSTICK && device->usage != USAGE_GAMEPAD && device->usage != USAGE_MULTIAXISCONTROLLER) {
            return NULL;
        }
    }

    for (i = 0; i < SDL_arraysize(SDL_HIDAPI_drivers); ++i) {
        SDL_HIDAPI_DeviceDriver *driver = SDL_HIDAPI_drivers[i];
        if (driver->enabled && driver->IsSupportedDevice(device, device->name, device->type, device->vendor_id, device->product_id, device->version, device->interface_number, device->interface_class, device->interface_subclass, device->interface_protocol)) {
            return driver;
        }
    }
    return NULL;
}

static SDL_HIDAPI_Device *HIDAPI_GetDeviceByIndex(int device_index, SDL_JoystickID *pJoystickID)
{
    SDL_HIDAPI_Device *device;

    SDL_AssertJoysticksLocked();

    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (device->parent) {
            continue;
        }
        if (device->driver) {
            if (device_index < device->num_joysticks) {
                if (pJoystickID) {
                    *pJoystickID = device->joysticks[device_index];
                }
                return device;
            }
            device_index -= device->num_joysticks;
        }
    }
    return NULL;
}

static SDL_HIDAPI_Device *HIDAPI_GetJoystickByInfo(const char *path, Uint16 vendor_id, Uint16 product_id)
{
    SDL_HIDAPI_Device *device;

    SDL_AssertJoysticksLocked();

    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (device->vendor_id == vendor_id && device->product_id == product_id &&
            SDL_strcmp(device->path, path) == 0) {
            break;
        }
    }
    return device;
}

static void HIDAPI_CleanupDeviceDriver(SDL_HIDAPI_Device *device)
{
    if (!device->driver) {
        return; /* Already cleaned up */
    }

    /* Disconnect any joysticks */
    while (device->num_joysticks && device->joysticks) {
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
    }

    device->driver->FreeDevice(device);
    device->driver = NULL;

    SDL_LockMutex(device->dev_lock);
    {
        if (device->dev) {
            SDL_hid_close(device->dev);
            device->dev = NULL;
        }

        if (device->context) {
            SDL_free(device->context);
            device->context = NULL;
        }
    }
    SDL_UnlockMutex(device->dev_lock);
}

static void HIDAPI_SetupDeviceDriver(SDL_HIDAPI_Device *device, SDL_bool *removed) SDL_NO_THREAD_SAFETY_ANALYSIS /* We unlock the joystick lock to be able to open the HID device on Android */
{
    *removed = SDL_FALSE;

    if (device->driver) {
        SDL_bool enabled;

        if (device->vendor_id == USB_VENDOR_NINTENDO && device->product_id == USB_PRODUCT_NINTENDO_SWITCH_JOYCON_PAIR) {
            enabled = SDL_HIDAPI_combine_joycons;
        } else {
            enabled = device->driver->enabled;
        }
        if (device->children) {
            int i;

            for (i = 0; i < device->num_children; ++i) {
                SDL_HIDAPI_Device *child = device->children[i];
                if (!child->driver || !child->driver->enabled) {
                    enabled = SDL_FALSE;
                    break;
                }
            }
        }
        if (!enabled) {
            HIDAPI_CleanupDeviceDriver(device);
        }
        return; /* Already setup */
    }

    if (HIDAPI_GetDeviceDriver(device)) {
        /* We might have a device driver for this device, try opening it and see */
        if (device->num_children == 0) {
            SDL_hid_device *dev;

            /* Wait a little bit for the device to initialize */
            SDL_Delay(10);

#ifdef __ANDROID__
            /* On Android we need to leave joysticks unlocked because it calls
             * out to the main thread for permissions and the main thread can
             * be in the process of handling controller input.
             *
             * See https://github.com/libsdl-org/SDL/issues/6347 for details
             */
            {
                SDL_HIDAPI_Device *curr;
                int lock_count = 0;
                char *path = SDL_strdup(device->path);

                SDL_AssertJoysticksLocked();
                while (SDL_JoysticksLocked()) {
                    ++lock_count;
                    SDL_UnlockJoysticks();
                }

                dev = SDL_hid_open_path(path, 0);

                while (lock_count > 0) {
                    --lock_count;
                    SDL_LockJoysticks();
                }
                SDL_free(path);

                /* Make sure the device didn't get removed while opening the HID path */
                for (curr = SDL_HIDAPI_devices; curr && curr != device; curr = curr->next) {
                    continue;
                }
                if (curr == NULL) {
                    *removed = SDL_TRUE;
                    if (dev) {
                        SDL_hid_close(dev);
                    }
                    return;
                }
            }
#else
            /* On other platforms we want to keep the lock so other threads wait for
             * us to finish opening the controller before checking to see whether the
             * HIDAPI driver is handling the device.
             *
             * On Windows, for example, the main thread can be enumerating DirectInput
             * devices while the Windows.Gaming.Input thread is calling back with a new
             * controller available.
             *
             * See https://github.com/libsdl-org/SDL/issues/7304 for details.
             */
            dev = SDL_hid_open_path(device->path, 0);
#endif

            if (dev == NULL) {
                SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                             "HIDAPI_SetupDeviceDriver() couldn't open %s: %s\n",
                             device->path, SDL_GetError());
                return;
            }
            SDL_hid_set_nonblocking(dev, 1);

            device->dev = dev;
        }

        device->driver = HIDAPI_GetDeviceDriver(device);

        /* Initialize the device, which may cause a connected event */
        if (device->driver && !device->driver->InitDevice(device)) {
            HIDAPI_CleanupDeviceDriver(device);
        }

        if (!device->driver && device->dev) {
            /* No driver claimed this device, go ahead and close it */
            SDL_hid_close(device->dev);
            device->dev = NULL;
        }
    }
}

static void SDL_HIDAPI_UpdateDrivers(void)
{
    int i;
    SDL_HIDAPI_Device *device;
    SDL_bool removed;

    SDL_AssertJoysticksLocked();

    SDL_HIDAPI_numdrivers = 0;
    for (i = 0; i < SDL_arraysize(SDL_HIDAPI_drivers); ++i) {
        SDL_HIDAPI_DeviceDriver *driver = SDL_HIDAPI_drivers[i];
        driver->enabled = driver->IsEnabled();
        if (driver->enabled && driver != &SDL_HIDAPI_DriverCombined) {
            ++SDL_HIDAPI_numdrivers;
        }
    }

    removed = SDL_FALSE;
    do {
        for (device = SDL_HIDAPI_devices; device; device = device->next) {
            HIDAPI_SetupDeviceDriver(device, &removed);
            if (removed) {
                break;
            }
        }
    } while (removed);
}

static void SDLCALL SDL_HIDAPIDriverHintChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    if (SDL_strcmp(name, SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS) == 0) {
        SDL_HIDAPI_combine_joycons = SDL_GetStringBoolean(hint, SDL_TRUE);
    }
    SDL_HIDAPI_hints_changed = SDL_TRUE;
    SDL_HIDAPI_change_count = 0;
}

static int HIDAPI_JoystickInit(void)
{
    int i;

    if (initialized) {
        return 0;
    }

#ifdef SDL_USE_LIBUDEV
    if (linux_enumeration_method == ENUMERATION_UNSET) {
        if (SDL_getenv("SDL_HIDAPI_JOYSTICK_DISABLE_UDEV") != NULL) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                         "udev disabled by SDL_HIDAPI_JOYSTICK_DISABLE_UDEV");
            linux_enumeration_method = ENUMERATION_FALLBACK;
        } else if (SDL_DetectSandbox() != SDL_SANDBOX_NONE) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                         "Container detected, disabling HIDAPI udev integration");
            linux_enumeration_method = ENUMERATION_FALLBACK;
        } else {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                         "Using udev for HIDAPI joystick device discovery");
            linux_enumeration_method = ENUMERATION_LIBUDEV;
        }
    }
#endif

    if (SDL_hid_init() < 0) {
        return SDL_SetError("Couldn't initialize hidapi");
    }

    for (i = 0; i < SDL_arraysize(SDL_HIDAPI_drivers); ++i) {
        SDL_HIDAPI_DeviceDriver *driver = SDL_HIDAPI_drivers[i];
        driver->RegisterHints(SDL_HIDAPIDriverHintChanged, driver);
    }
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS,
                        SDL_HIDAPIDriverHintChanged, NULL);
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI,
                        SDL_HIDAPIDriverHintChanged, NULL);

    SDL_HIDAPI_change_count = SDL_hid_device_change_count();
    HIDAPI_UpdateDeviceList();
    HIDAPI_UpdateDevices();

    initialized = SDL_TRUE;

    return 0;
}

static SDL_bool HIDAPI_AddJoystickInstanceToDevice(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID)
{
    SDL_JoystickID *joysticks = (SDL_JoystickID *)SDL_realloc(device->joysticks, (device->num_joysticks + 1) * sizeof(*device->joysticks));
    if (joysticks == NULL) {
        return SDL_FALSE;
    }

    device->joysticks = joysticks;
    device->joysticks[device->num_joysticks++] = joystickID;
    return SDL_TRUE;
}

static SDL_bool HIDAPI_DelJoystickInstanceFromDevice(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID)
{
    int i, size;

    for (i = 0; i < device->num_joysticks; ++i) {
        if (device->joysticks[i] == joystickID) {
            size = (device->num_joysticks - i - 1) * sizeof(SDL_JoystickID);
            SDL_memmove(&device->joysticks[i], &device->joysticks[i + 1], size);
            --device->num_joysticks;
            if (device->num_joysticks == 0) {
                SDL_free(device->joysticks);
                device->joysticks = NULL;
            }
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

static SDL_bool HIDAPI_JoystickInstanceIsUnique(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID)
{
    if (device->parent && device->num_joysticks == 1 && device->parent->num_joysticks == 1 &&
        device->joysticks[0] == device->parent->joysticks[0]) {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

void HIDAPI_SetDeviceName(SDL_HIDAPI_Device *device, const char *name)
{
    if (name && *name && SDL_strcmp(name, device->name) != 0) {
        SDL_free(device->name);
        device->name = SDL_strdup(name);
        SDL_SetJoystickGUIDCRC(&device->guid, SDL_crc16(0, name, SDL_strlen(name)));
    }
}

void HIDAPI_SetDeviceProduct(SDL_HIDAPI_Device *device, Uint16 product_id)
{
    /* Don't set the device product ID directly, or we'll constantly re-enumerate this device */
    SDL_SetJoystickGUIDProduct(&device->guid, product_id);
}

static void HIDAPI_UpdateJoystickSerial(SDL_HIDAPI_Device *device)
{
    int i;

    for (i = 0; i < device->num_joysticks; ++i) {
        SDL_Joystick *joystick = SDL_GetJoystickFromInstanceID(device->joysticks[i]);
        if (joystick && device->serial) {
            SDL_free(joystick->serial);
            joystick->serial = SDL_strdup(device->serial);
        }
    }
}

void HIDAPI_SetDeviceSerial(SDL_HIDAPI_Device *device, const char *serial)
{
    if (serial && *serial && (!device->serial || SDL_strcmp(serial, device->serial) != 0)) {
        SDL_free(device->serial);
        device->serial = SDL_strdup(serial);
        HIDAPI_UpdateJoystickSerial(device);
    }
}

static int wcstrcmp(const wchar_t *str1, const char *str2)
{
    int result;

    while (1) {
        result = (*str1 - *str2);
        if (result != 0 || *str1 == 0) {
            break;
        }
        ++str1;
        ++str2;
    }
    return result;
}

static void HIDAPI_SetDeviceSerialW(SDL_HIDAPI_Device *device, const wchar_t *serial)
{
    if (serial && *serial && (!device->serial || wcstrcmp(serial, device->serial) != 0)) {
        SDL_free(device->serial);
        device->serial = HIDAPI_ConvertString(serial);
        HIDAPI_UpdateJoystickSerial(device);
    }
}

SDL_bool HIDAPI_HasConnectedUSBDevice(const char *serial)
{
    SDL_HIDAPI_Device *device;

    SDL_AssertJoysticksLocked();

    if (serial == NULL) {
        return SDL_FALSE;
    }

    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (!device->driver) {
            continue;
        }

        if (device->is_bluetooth) {
            continue;
        }

        if (device->serial && SDL_strcmp(serial, device->serial) == 0) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

void HIDAPI_DisconnectBluetoothDevice(const char *serial)
{
    SDL_HIDAPI_Device *device;

    SDL_AssertJoysticksLocked();

    if (serial == NULL) {
        return;
    }

    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (!device->driver) {
            continue;
        }

        if (!device->is_bluetooth) {
            continue;
        }

        if (device->serial && SDL_strcmp(serial, device->serial) == 0) {
            while (device->num_joysticks && device->joysticks) {
                HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
            }
        }
    }
}

SDL_bool HIDAPI_JoystickConnected(SDL_HIDAPI_Device *device, SDL_JoystickID *pJoystickID)
{
    int i, j;
    SDL_JoystickID joystickID;

    SDL_AssertJoysticksLocked();

    for (i = 0; i < device->num_children; ++i) {
        SDL_HIDAPI_Device *child = device->children[i];
        for (j = child->num_joysticks; j--;) {
            HIDAPI_JoystickDisconnected(child, child->joysticks[j]);
        }
    }

    joystickID = SDL_GetNextJoystickInstanceID();
    HIDAPI_AddJoystickInstanceToDevice(device, joystickID);

    for (i = 0; i < device->num_children; ++i) {
        SDL_HIDAPI_Device *child = device->children[i];
        HIDAPI_AddJoystickInstanceToDevice(child, joystickID);
    }

    ++SDL_HIDAPI_numjoysticks;

    SDL_PrivateJoystickAdded(joystickID);

    if (pJoystickID) {
        *pJoystickID = joystickID;
    }
    return SDL_TRUE;
}

void HIDAPI_JoystickDisconnected(SDL_HIDAPI_Device *device, SDL_JoystickID joystickID)
{
    int i, j;

    SDL_LockJoysticks();

    if (!HIDAPI_JoystickInstanceIsUnique(device, joystickID)) {
        /* Disconnecting a child always disconnects the parent */
        device = device->parent;
    }

    for (i = 0; i < device->num_joysticks; ++i) {
        if (device->joysticks[i] == joystickID) {
            SDL_Joystick *joystick = SDL_GetJoystickFromInstanceID(joystickID);
            if (joystick) {
                HIDAPI_JoystickClose(joystick);
            }

            HIDAPI_DelJoystickInstanceFromDevice(device, joystickID);

            for (j = 0; j < device->num_children; ++j) {
                SDL_HIDAPI_Device *child = device->children[j];
                HIDAPI_DelJoystickInstanceFromDevice(child, joystickID);
            }

            --SDL_HIDAPI_numjoysticks;

            if (!shutting_down) {
                SDL_PrivateJoystickRemoved(joystickID);
            }
        }
    }

    /* Rescan the device list in case device state has changed */
    SDL_HIDAPI_change_count = 0;

    SDL_UnlockJoysticks();
}

static int HIDAPI_JoystickGetCount(void)
{
    return SDL_HIDAPI_numjoysticks;
}

static SDL_HIDAPI_Device *HIDAPI_AddDevice(const struct SDL_hid_device_info *info, int num_children, SDL_HIDAPI_Device **children)
{
    SDL_HIDAPI_Device *device;
    SDL_HIDAPI_Device *curr, *last = NULL;
    SDL_bool removed;

    SDL_AssertJoysticksLocked();

    for (curr = SDL_HIDAPI_devices, last = NULL; curr; last = curr, curr = curr->next) {
    }

    device = (SDL_HIDAPI_Device *)SDL_calloc(1, sizeof(*device));
    if (device == NULL) {
        return NULL;
    }
    device->magic = &SDL_HIDAPI_device_magic;
    device->path = SDL_strdup(info->path);
    if (!device->path) {
        SDL_free(device);
        return NULL;
    }
    device->seen = SDL_TRUE;
    device->vendor_id = info->vendor_id;
    device->product_id = info->product_id;
    device->version = info->release_number;
    device->interface_number = info->interface_number;
    device->interface_class = info->interface_class;
    device->interface_subclass = info->interface_subclass;
    device->interface_protocol = info->interface_protocol;
    device->usage_page = info->usage_page;
    device->usage = info->usage;
    device->dev_lock = SDL_CreateMutex();

    /* Need the device name before getting the driver to know whether to ignore this device */
    {
        char *manufacturer_string = HIDAPI_ConvertString(info->manufacturer_string);
        char *product_string = HIDAPI_ConvertString(info->product_string);
        char *serial_number = HIDAPI_ConvertString(info->serial_number);

        device->name = SDL_CreateJoystickName(device->vendor_id, device->product_id, manufacturer_string, product_string);

        if (manufacturer_string) {
            SDL_free(manufacturer_string);
        }
        if (product_string) {
            SDL_free(product_string);
        }

        if (serial_number && *serial_number) {
            device->serial = serial_number;
        } else {
            SDL_free(serial_number);
        }

        if (!device->name) {
            SDL_free(device->serial);
            SDL_free(device->path);
            SDL_free(device);
            return NULL;
        }
    }

    /* FIXME: Is there any way to tell whether this is a Bluetooth device? */
    device->guid = SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, device->vendor_id, device->product_id, device->version, device->name, 'h', 0);
    device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
    device->type = SDL_GetJoystickGameControllerProtocol(device->name, device->vendor_id, device->product_id, device->interface_number, device->interface_class, device->interface_subclass, device->interface_protocol);

    if (num_children > 0) {
        int i;

        device->num_children = num_children;
        device->children = children;
        for (i = 0; i < num_children; ++i) {
            children[i]->parent = device;
        }
    }

    /* Add it to the list */
    if (last) {
        last->next = device;
    } else {
        SDL_HIDAPI_devices = device;
    }

    removed = SDL_FALSE;
    HIDAPI_SetupDeviceDriver(device, &removed);
    if (removed) {
        return NULL;
    }

#ifdef DEBUG_HIDAPI
    SDL_Log("Added HIDAPI device '%s' VID 0x%.4x, PID 0x%.4x, version %d, serial %s, interface %d, interface_class %d, interface_subclass %d, interface_protocol %d, usage page 0x%.4x, usage 0x%.4x, path = %s, driver = %s (%s)\n", device->name, device->vendor_id, device->product_id, device->version, device->serial ? device->serial : "NONE", device->interface_number, device->interface_class, device->interface_subclass, device->interface_protocol, device->usage_page, device->usage, device->path, device->driver ? device->driver->name : "NONE", device->driver && device->driver->enabled ? "ENABLED" : "DISABLED");
#endif

    return device;
}

static void HIDAPI_DelDevice(SDL_HIDAPI_Device *device)
{
    SDL_HIDAPI_Device *curr, *last;
    int i;

    SDL_AssertJoysticksLocked();

#ifdef DEBUG_HIDAPI
    SDL_Log("Removing HIDAPI device '%s' VID 0x%.4x, PID 0x%.4x, version %d, serial %s, interface %d, interface_class %d, interface_subclass %d, interface_protocol %d, usage page 0x%.4x, usage 0x%.4x, path = %s, driver = %s (%s)\n", device->name, device->vendor_id, device->product_id, device->version, device->serial ? device->serial : "NONE", device->interface_number, device->interface_class, device->interface_subclass, device->interface_protocol, device->usage_page, device->usage, device->path, device->driver ? device->driver->name : "NONE", device->driver && device->driver->enabled ? "ENABLED" : "DISABLED");
#endif

    for (curr = SDL_HIDAPI_devices, last = NULL; curr; last = curr, curr = curr->next) {
        if (curr == device) {
            if (last) {
                last->next = curr->next;
            } else {
                SDL_HIDAPI_devices = curr->next;
            }

            HIDAPI_CleanupDeviceDriver(device);

            /* Make sure the rumble thread is done with this device */
            while (SDL_AtomicGet(&device->rumble_pending) > 0) {
                SDL_Delay(10);
            }

            for (i = 0; i < device->num_children; ++i) {
                device->children[i]->parent = NULL;
            }

            device->magic = NULL;
            SDL_DestroyMutex(device->dev_lock);
            SDL_free(device->serial);
            SDL_free(device->name);
            SDL_free(device->path);
            SDL_free(device->children);
            SDL_free(device);
            return;
        }
    }
}

static SDL_bool HIDAPI_CreateCombinedJoyCons(void)
{
    SDL_HIDAPI_Device *device, *combined;
    SDL_HIDAPI_Device *joycons[2] = { NULL, NULL };

    SDL_AssertJoysticksLocked();

    if (!SDL_HIDAPI_combine_joycons) {
        return SDL_FALSE;
    }

    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        Uint16 vendor, product;

        if (!device->driver) {
            /* Unsupported device */
            continue;
        }
        if (device->parent) {
            /* This device is already part of a combined device */
            continue;
        }

        SDL_GetJoystickGUIDInfo(device->guid, &vendor, &product, NULL, NULL);

        if (!joycons[0] &&
            (SDL_IsJoystickNintendoSwitchJoyConLeft(vendor, product) ||
             (SDL_IsJoystickNintendoSwitchJoyConGrip(vendor, product) &&
              SDL_strstr(device->name, "(L)") != NULL))) {
            joycons[0] = device;
        }
        if (!joycons[1] &&
            (SDL_IsJoystickNintendoSwitchJoyConRight(vendor, product) ||
             (SDL_IsJoystickNintendoSwitchJoyConGrip(vendor, product) &&
              SDL_strstr(device->name, "(R)") != NULL))) {
            joycons[1] = device;
        }
        if (joycons[0] && joycons[1]) {
            SDL_hid_device_info info;
            SDL_HIDAPI_Device **children = (SDL_HIDAPI_Device **)SDL_malloc(2 * sizeof(SDL_HIDAPI_Device *));
            if (children == NULL) {
                return SDL_FALSE;
            }
            children[0] = joycons[0];
            children[1] = joycons[1];

            SDL_zero(info);
            info.path = "nintendo_joycons_combined";
            info.vendor_id = USB_VENDOR_NINTENDO;
            info.product_id = USB_PRODUCT_NINTENDO_SWITCH_JOYCON_PAIR;
            info.interface_number = -1;
            info.usage_page = USB_USAGEPAGE_GENERIC_DESKTOP;
            info.usage = USB_USAGE_GENERIC_GAMEPAD;
            info.manufacturer_string = L"Nintendo";
            info.product_string = L"Switch Joy-Con (L/R)";

            combined = HIDAPI_AddDevice(&info, 2, children);
            if (combined && combined->driver) {
                return SDL_TRUE;
            } else {
                if (combined) {
                    HIDAPI_DelDevice(combined);
                } else {
                    SDL_free(children);
                }
                return SDL_FALSE;
            }
        }
    }
    return SDL_FALSE;
}

static void HIDAPI_UpdateDeviceList(void)
{
    SDL_HIDAPI_Device *device;
    struct SDL_hid_device_info *devs, *info;

    SDL_LockJoysticks();

    if (SDL_HIDAPI_hints_changed) {
        SDL_HIDAPI_UpdateDrivers();
        SDL_HIDAPI_hints_changed = SDL_FALSE;
    }

    /* Prepare the existing device list */
    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (device->children) {
            continue;
        }
        device->seen = SDL_FALSE;
    }

    /* Enumerate the devices */
    if (SDL_HIDAPI_numdrivers > 0) {
        devs = SDL_hid_enumerate(0, 0);
        if (devs) {
            for (info = devs; info; info = info->next) {
                device = HIDAPI_GetJoystickByInfo(info->path, info->vendor_id, info->product_id);
                if (device) {
                    device->seen = SDL_TRUE;

                    /* Check to see if the serial number is available now */
                    HIDAPI_SetDeviceSerialW(device, info->serial_number);
                } else {
                    HIDAPI_AddDevice(info, 0, NULL);
                }
            }
            SDL_hid_free_enumeration(devs);
        }
    }

    /* Remove any devices that weren't seen or have been disconnected due to read errors */
check_removed:
    device = SDL_HIDAPI_devices;
    while (device) {
        SDL_HIDAPI_Device *next = device->next;

        if (!device->seen ||
            ((device->driver || device->children) && device->num_joysticks == 0 && !device->dev)) {
            if (device->parent) {
                /* When a child device goes away, so does the parent */
                int i;
                device = device->parent;
                for (i = 0; i < device->num_children; ++i) {
                    HIDAPI_DelDevice(device->children[i]);
                }
                HIDAPI_DelDevice(device);

                /* Update the device list again to pick up any children left */
                SDL_HIDAPI_change_count = 0;

                /* We deleted more than one device here, restart the loop */
                goto check_removed;
            } else {
                HIDAPI_DelDevice(device);

                /* Update the device list again in case this device comes back */
                SDL_HIDAPI_change_count = 0;
            }
        }
        device = next;
    }

    /* See if we can create any combined Joy-Con controllers */
    while (HIDAPI_CreateCombinedJoyCons()) {
    }

    SDL_UnlockJoysticks();
}

static SDL_bool HIDAPI_IsEquivalentToDevice(Uint16 vendor_id, Uint16 product_id, SDL_HIDAPI_Device *device)
{
    if (vendor_id == device->vendor_id && product_id == device->product_id) {
        return SDL_TRUE;
    }

    if (vendor_id == USB_VENDOR_MICROSOFT) {
        /* If we're looking for the wireless XBox 360 controller, also look for the dongle */
        if (product_id == USB_PRODUCT_XBOX360_XUSB_CONTROLLER && device->product_id == USB_PRODUCT_XBOX360_WIRELESS_RECEIVER) {
            return SDL_TRUE;
        }

        /* If we're looking for the raw input Xbox One controller, match it against any other Xbox One controller */
        if (product_id == USB_PRODUCT_XBOX_ONE_XBOXGIP_CONTROLLER &&
            device->type == SDL_GAMEPAD_TYPE_XBOXONE) {
            return SDL_TRUE;
        }

        /* If we're looking for an XInput controller, match it against any other Xbox controller */
        if (product_id == USB_PRODUCT_XBOX_ONE_XINPUT_CONTROLLER) {
            if (device->type == SDL_GAMEPAD_TYPE_XBOX360 || device->type == SDL_GAMEPAD_TYPE_XBOXONE) {
                return SDL_TRUE;
            }
        }
    }

    if (vendor_id == USB_VENDOR_NVIDIA) {
        /* If we're looking for the NVIDIA SHIELD controller Xbox interface, match it against any NVIDIA SHIELD controller */
        if (product_id == 0xb400 &&
            device->type == SDL_GAMEPAD_TYPE_NVIDIA_SHIELD) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

SDL_bool HIDAPI_IsDeviceTypePresent(SDL_GamepadType type)
{
    SDL_HIDAPI_Device *device;
    SDL_bool result = SDL_FALSE;

    /* Make sure we're initialized, as this could be called from other drivers during startup */
    if (HIDAPI_JoystickInit() < 0) {
        return SDL_FALSE;
    }

    if (SDL_AtomicTryLock(&SDL_HIDAPI_spinlock)) {
        HIDAPI_UpdateDeviceList();
        SDL_AtomicUnlock(&SDL_HIDAPI_spinlock);
    }

    SDL_LockJoysticks();
    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (device->driver && device->type == type) {
            result = SDL_TRUE;
            break;
        }
    }
    SDL_UnlockJoysticks();

#ifdef DEBUG_HIDAPI
    SDL_Log("HIDAPI_IsDeviceTypePresent() returning %s for %d\n", result ? "true" : "false", type);
#endif
    return result;
}

SDL_bool HIDAPI_IsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    SDL_HIDAPI_Device *device;
    SDL_bool supported = SDL_FALSE;
    SDL_bool result = SDL_FALSE;

    /* Make sure we're initialized, as this could be called from other drivers during startup */
    if (HIDAPI_JoystickInit() < 0) {
        return SDL_FALSE;
    }

    /* Only update the device list for devices we know might be supported.
       If we did this for every device, it would hit the USB driver too hard and potentially
       lock up the system. This won't catch devices that we support but can only detect using
       USB interface details, like Xbox controllers, but hopefully the device list update is
       responsive enough to catch those.
     */
    supported = HIDAPI_IsDeviceSupported(vendor_id, product_id, version, name);
#if defined(SDL_JOYSTICK_HIDAPI_XBOX360) || defined(SDL_JOYSTICK_HIDAPI_XBOXONE)
    if (!supported &&
        (SDL_strstr(name, "Xbox") || SDL_strstr(name, "X-Box") || SDL_strstr(name, "XBOX"))) {
        supported = SDL_TRUE;
    }
#endif /* SDL_JOYSTICK_HIDAPI_XBOX360 || SDL_JOYSTICK_HIDAPI_XBOXONE */
    if (supported) {
        if (SDL_AtomicTryLock(&SDL_HIDAPI_spinlock)) {
            HIDAPI_UpdateDeviceList();
            SDL_AtomicUnlock(&SDL_HIDAPI_spinlock);
        }
    }

    /* Note that this isn't a perfect check - there may be multiple devices with 0 VID/PID,
       or a different name than we have it listed here, etc, but if we support the device
       and we have something similar in our device list, mark it as present.
     */
    SDL_LockJoysticks();
    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (device->driver &&
            HIDAPI_IsEquivalentToDevice(vendor_id, product_id, device)) {
            result = SDL_TRUE;
            break;
        }
    }
    SDL_UnlockJoysticks();

#ifdef DEBUG_HIDAPI
    SDL_Log("HIDAPI_IsDevicePresent() returning %s for 0x%.4x / 0x%.4x\n", result ? "true" : "false", vendor_id, product_id);
#endif
    return result;
}

SDL_JoystickType HIDAPI_GetJoystickTypeFromGUID(SDL_JoystickGUID guid)
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickType type = SDL_JOYSTICK_TYPE_UNKNOWN;

    SDL_LockJoysticks();
    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (SDL_memcmp(&guid, &device->guid, sizeof(guid)) == 0) {
            type = device->joystick_type;
            break;
        }
    }
    SDL_UnlockJoysticks();

    return type;
}

SDL_GamepadType HIDAPI_GetGamepadTypeFromGUID(SDL_JoystickGUID guid)
{
    SDL_HIDAPI_Device *device;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;

    SDL_LockJoysticks();
    for (device = SDL_HIDAPI_devices; device; device = device->next) {
        if (SDL_memcmp(&guid, &device->guid, sizeof(guid)) == 0) {
            type = device->type;
            break;
        }
    }
    SDL_UnlockJoysticks();

    return type;
}

static void HIDAPI_JoystickDetect(void)
{
    if (SDL_AtomicTryLock(&SDL_HIDAPI_spinlock)) {
        Uint32 count = SDL_hid_device_change_count();
        if (SDL_HIDAPI_change_count != count) {
            SDL_HIDAPI_change_count = count;
            HIDAPI_UpdateDeviceList();
        }
        SDL_AtomicUnlock(&SDL_HIDAPI_spinlock);
    }
}

void HIDAPI_UpdateDevices(void)
{
    SDL_HIDAPI_Device *device;

    SDL_AssertJoysticksLocked();

    /* Update the devices, which may change connected joysticks and send events */

    /* Prepare the existing device list */
    if (SDL_AtomicTryLock(&SDL_HIDAPI_spinlock)) {
        for (device = SDL_HIDAPI_devices; device; device = device->next) {
            if (device->parent) {
                continue;
            }
            if (device->driver) {
                if (SDL_TryLockMutex(device->dev_lock) == 0) {
                    device->updating = SDL_TRUE;
                    device->driver->UpdateDevice(device);
                    device->updating = SDL_FALSE;
                    SDL_UnlockMutex(device->dev_lock);
                }
            }
        }
        SDL_AtomicUnlock(&SDL_HIDAPI_spinlock);
    }
}

static const char *HIDAPI_JoystickGetDeviceName(int device_index)
{
    SDL_HIDAPI_Device *device;
    const char *name = NULL;

    device = HIDAPI_GetDeviceByIndex(device_index, NULL);
    if (device) {
        /* FIXME: The device could be freed after this name is returned... */
        name = device->name;
    }

    return name;
}

static const char *HIDAPI_JoystickGetDevicePath(int device_index)
{
    SDL_HIDAPI_Device *device;
    const char *path = NULL;

    device = HIDAPI_GetDeviceByIndex(device_index, NULL);
    if (device) {
        /* FIXME: The device could be freed after this path is returned... */
        path = device->path;
    }

    return path;
}

static int HIDAPI_JoystickGetDevicePlayerIndex(int device_index)
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickID instance_id;
    int player_index = -1;

    device = HIDAPI_GetDeviceByIndex(device_index, &instance_id);
    if (device) {
        player_index = device->driver->GetDevicePlayerIndex(device, instance_id);
    }

    return player_index;
}

static void HIDAPI_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickID instance_id;

    device = HIDAPI_GetDeviceByIndex(device_index, &instance_id);
    if (device) {
        device->driver->SetDevicePlayerIndex(device, instance_id, player_index);
    }
}

static SDL_JoystickGUID HIDAPI_JoystickGetDeviceGUID(int device_index)
{
    SDL_HIDAPI_Device *device;
    SDL_JoystickGUID guid;

    device = HIDAPI_GetDeviceByIndex(device_index, NULL);
    if (device) {
        SDL_memcpy(&guid, &device->guid, sizeof(guid));
    } else {
        SDL_zero(guid);
    }

    return guid;
}

static SDL_JoystickID HIDAPI_JoystickGetDeviceInstanceID(int device_index)
{
    SDL_JoystickID joystickID = 0;
    HIDAPI_GetDeviceByIndex(device_index, &joystickID);
    return joystickID;
}

static int HIDAPI_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    SDL_JoystickID joystickID = 0;
    SDL_HIDAPI_Device *device = HIDAPI_GetDeviceByIndex(device_index, &joystickID);
    struct joystick_hwdata *hwdata;

    SDL_AssertJoysticksLocked();

    if (device == NULL || !device->driver) {
        /* This should never happen - validated before being called */
        return SDL_SetError("Couldn't find HIDAPI device at index %d\n", device_index);
    }

    hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (hwdata == NULL) {
        return SDL_OutOfMemory();
    }
    hwdata->device = device;

    /* Process any pending reports before opening the device */
    SDL_LockMutex(device->dev_lock);
    device->updating = SDL_TRUE;
    device->driver->UpdateDevice(device);
    device->updating = SDL_FALSE;
    SDL_UnlockMutex(device->dev_lock);

    if (!device->driver->OpenJoystick(device, joystick)) {
        /* The open failed, mark this device as disconnected and update devices */
        HIDAPI_JoystickDisconnected(device, joystickID);
        SDL_free(hwdata);
        return -1;
    }

    if (device->serial) {
        joystick->serial = SDL_strdup(device->serial);
    }

    joystick->hwdata = hwdata;
    return 0;
}

static SDL_bool HIDAPI_GetJoystickDevice(SDL_Joystick *joystick, SDL_HIDAPI_Device **device)
{
    SDL_AssertJoysticksLocked();

    if (joystick && joystick->hwdata) {
        *device = joystick->hwdata->device;
        if (*device && (*device)->magic == &SDL_HIDAPI_device_magic && (*device)->driver != NULL) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

static int HIDAPI_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    int result;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->RumbleJoystick(device, joystick, low_frequency_rumble, high_frequency_rumble);
    } else {
        result = SDL_SetError("Rumble failed, device disconnected");
    }

    return result;
}

static int HIDAPI_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    int result;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->RumbleJoystickTriggers(device, joystick, left_rumble, right_rumble);
    } else {
        result = SDL_SetError("Rumble failed, device disconnected");
    }

    return result;
}

static Uint32 HIDAPI_JoystickGetCapabilities(SDL_Joystick *joystick)
{
    Uint32 result = 0;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->GetJoystickCapabilities(device, joystick);
    }

    return result;
}

static int HIDAPI_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    int result;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->SetJoystickLED(device, joystick, red, green, blue);
    } else {
        result = SDL_SetError("SetLED failed, device disconnected");
    }

    return result;
}

static int HIDAPI_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    int result;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->SendJoystickEffect(device, joystick, data, size);
    } else {
        result = SDL_SetError("SendEffect failed, device disconnected");
    }

    return result;
}

static int HIDAPI_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled)
{
    int result;
    SDL_HIDAPI_Device *device = NULL;

    if (HIDAPI_GetJoystickDevice(joystick, &device)) {
        result = device->driver->SetJoystickSensorsEnabled(device, joystick, enabled);
    } else {
        result = SDL_SetError("SetSensorsEnabled failed, device disconnected");
    }

    return result;
}

static void HIDAPI_JoystickUpdate(SDL_Joystick *joystick)
{
    /* This is handled in SDL_HIDAPI_UpdateDevices() */
}

static void HIDAPI_JoystickClose(SDL_Joystick *joystick) SDL_NO_THREAD_SAFETY_ANALYSIS /* We unlock the device lock so rumble can complete */
{
    SDL_AssertJoysticksLocked();

    if (joystick->hwdata) {
        SDL_HIDAPI_Device *device = joystick->hwdata->device;
        int i;

        /* Wait up to 30 ms for pending rumble to complete */
        if (device->updating) {
            /* Unlock the device so rumble can complete */
            SDL_UnlockMutex(device->dev_lock);
        }
        for (i = 0; i < 3; ++i) {
            if (SDL_AtomicGet(&device->rumble_pending) > 0) {
                SDL_Delay(10);
            }
        }
        if (device->updating) {
            /* Relock the device */
            SDL_LockMutex(device->dev_lock);
        }

        device->driver->CloseJoystick(device, joystick);

        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
    }
}

static void HIDAPI_JoystickQuit(void)
{
    int i;

    SDL_AssertJoysticksLocked();

    shutting_down = SDL_TRUE;

    SDL_HIDAPI_QuitRumble();

    while (SDL_HIDAPI_devices) {
        SDL_HIDAPI_Device *device = SDL_HIDAPI_devices;
        if (device->parent) {
            /* When a child device goes away, so does the parent */
            device = device->parent;
            for (i = 0; i < device->num_children; ++i) {
                HIDAPI_DelDevice(device->children[i]);
            }
            HIDAPI_DelDevice(device);
        } else {
            HIDAPI_DelDevice(device);
        }
    }

    /* Make sure the drivers cleaned up properly */
    SDL_assert(SDL_HIDAPI_numjoysticks == 0);

    for (i = 0; i < SDL_arraysize(SDL_HIDAPI_drivers); ++i) {
        SDL_HIDAPI_DeviceDriver *driver = SDL_HIDAPI_drivers[i];
        driver->UnregisterHints(SDL_HIDAPIDriverHintChanged, driver);
    }
    SDL_DelHintCallback(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS,
                        SDL_HIDAPIDriverHintChanged, NULL);
    SDL_DelHintCallback(SDL_HINT_JOYSTICK_HIDAPI,
                        SDL_HIDAPIDriverHintChanged, NULL);

    SDL_hid_exit();

    SDL_HIDAPI_change_count = 0;
    shutting_down = SDL_FALSE;
    initialized = SDL_FALSE;
}

static SDL_bool HIDAPI_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    return SDL_FALSE;
}

SDL_JoystickDriver SDL_HIDAPI_JoystickDriver = {
    HIDAPI_JoystickInit,
    HIDAPI_JoystickGetCount,
    HIDAPI_JoystickDetect,
    HIDAPI_JoystickGetDeviceName,
    HIDAPI_JoystickGetDevicePath,
    HIDAPI_JoystickGetDevicePlayerIndex,
    HIDAPI_JoystickSetDevicePlayerIndex,
    HIDAPI_JoystickGetDeviceGUID,
    HIDAPI_JoystickGetDeviceInstanceID,
    HIDAPI_JoystickOpen,
    HIDAPI_JoystickRumble,
    HIDAPI_JoystickRumbleTriggers,
    HIDAPI_JoystickGetCapabilities,
    HIDAPI_JoystickSetLED,
    HIDAPI_JoystickSendEffect,
    HIDAPI_JoystickSetSensorsEnabled,
    HIDAPI_JoystickUpdate,
    HIDAPI_JoystickClose,
    HIDAPI_JoystickQuit,
    HIDAPI_JoystickGetGamepadMapping
};

#endif /* SDL_JOYSTICK_HIDAPI */
