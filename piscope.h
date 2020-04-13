/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
*/

#ifndef PISCOPE_H
#define PISCOPE_H

/* from pigpio pigpio.h */

#define PI_ENVPORT "PIGPIO_PORT"
#define PI_ENVADDR "PIGPIO_ADDR"

#define PI_DEFAULT_SERVER_ADDRESS "localhost"
#define PI_DEFAULT_SOCKET_PORT 8888

#define SETTINGS_FILE_NAME "piscope.conf"
#define SETTINGS_GROUP "Settings"
#define SETTINGS_SERVER_ADDRESS "serverAddress"
#define SETTINGS_SERVER_PORT "serverPort"
#define SETTINGS_ACTIVE_GPIOS "activeGPIOs"
#define SETTINGS_TRIGGER_SAMPLES "triggerSamples"
#define SETTINGS_TRIGGER_ENABLED "trigger%dEnabled"
#define SETTINGS_TRIGGER_ACTION "trigger%dAction"
#define SETTINGS_TRIGGER_GPIO_TYPES "trigger%dGPIOTypes"

#define PI_CMD_HWVER 17
#define PI_CMD_NB    19
#define PI_CMD_NC    21
#define PI_CMD_NOIB  99

typedef struct
{
   uint16_t seqno;
   uint16_t flags;
   uint32_t tick;
   uint32_t level;
} gpioReport_t;

/* from pigpio command.h */

typedef struct
{
   uint32_t cmd;
   uint32_t p1;
   uint32_t p2;
   union
   {
      uint32_t p3;
      uint32_t ext_len;
      uint32_t res;
   };
} cmdCmd_t;

#endif
