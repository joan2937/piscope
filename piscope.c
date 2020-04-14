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

/*

piscope uses pigpiod to provide the raw data from the gpios.

http://abyz.me.uk/rpi/pigpio/index.html

On the Pi you need to start the pigpio daemon, e.g.

sudo pigpiod

If you are running piscope remotely (i.e. not on the Pi running pigpiod)
you can specify the Pi in one of the following ways.

1. set the environment variable PIGPIO_ADDR
   e.g.
   export PIGPIO_ADDR=soft # specify by host name
   or
   export PIGPIO_ADDR=192.168.1.67 # specify by IP address

2. set the address/hostname and port the piscope preferences dialog.

Note: if set, the PIGPIO_ADDR environment variable takes precedence.

*/

#define PISCOPE_VERSION "0.8"

#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <arpa/inet.h>

#include "piscope.h"

/* DEFINES ---------------------------------------------------------------- */

#define MY_STOCK_CANCEL "gtk-cancel"
#define MY_STOCK_OPEN   "gtk-open"
#define MY_STOCK_SAVE   "gtk-save"

#define PISCOPE_MILLION              1000000L
#define PISCOPE_TRIGGERS                    4
#define PISCOPE_GPIOS                      32
#define PISCOPE_SAMPLES               1000000
#define PISCOPE_MAX_REPORTS_PER_READ     1000
#define PISCOPE_MIN_SPEED_IDX               0
#define PISCOPE_DEF_SPEED_IDX               6
#define PISCOPE_MAX_SPEED_IDX              21
#define PISCOPE_DEFAULT_ZOOM_LEVEL         13

#define PISCOPE_BUILDOBJ(x) \
   x = (GtkWidget*) gtk_builder_get_object(builder, #x);

/* TYPES ------------------------------------------------------------------ */

typedef enum
{
   piscope_bad_send        = -1000,
   piscope_bad_recv        = -1001,
   piscope_bad_socket      = -1002,
   piscope_bad_connect     = -1003,
   piscope_bad_no          = -1004,
   piscope_bad_noib        = -1005,
   piscope_bad_npipe       = -1006,
   piscope_bad_nsock       = -1007,
   piscope_bad_nb          = -1008,
   piscope_bad_report      = -1009,
   piscope_bad_getaddrinfo = -1010
} piscopeError_t;

typedef enum
{
   piscope_vcd  = 0,
   piscope_text = 1,
} piscopFileType_t;

typedef enum
{
   piscope_live  = 0,
   piscope_play  = 1,
   piscope_pause = 2
} piscopeMode_t;

typedef enum
{
   piscope_initialise = 0,
   piscope_running    = 1,
   piscope_dormant    = 2,
   piscope_quit       = 3
} piscopeState_t;

typedef enum
{
   piscope_dont_care = 0,
   piscope_low       = 1,
   piscope_high      = 2,
   piscope_edge      = 3,
   piscope_falling   = 4,
   piscope_rising    = 5
} piscopeTrigType_t;

typedef enum
{
   piscope_count         = 0,
   piscope_sample_from   = 1,
   piscope_sample_around = 2,
   piscope_sample_to     = 3
} piscopeTrigWhen_t;

typedef struct
{
   uint64_t          count;
   uint32_t          levelMask;
   uint32_t          changedMask;
   uint32_t          levelValue;
   int               enabled;
   int               fired;
   piscopeTrigWhen_t when;
   piscopeTrigType_t type[PISCOPE_GPIOS];
   GtkWidget         *onW;
   GtkWidget         *labelW;
   GtkWidget         *whenW;
} piscopeTrigInfo_t;

typedef struct
{
   int             display;
   int             y_low;
   int             y_high;
   int             y_tick;
   int             hilit;
   GtkToggleButton *button;
   char            *name;
} piscopeGpioInfo_t;

typedef struct
{
   int  usable;
   char *name;
} piscopeGpioUsage_t;

typedef struct
{
   gboolean enabled;
   gint action;
   gint gpiotypes[PISCOPE_GPIOS];
} piscopeTriggerSettings_t;

typedef struct
{
   gchar *serverAddress;
   gint  *activeGPIOs;
   gsize activeGPIOCount;
   gint  port;
   gint triggerSamples;
   piscopeTriggerSettings_t triggers[PISCOPE_TRIGGERS];
} piscopeSettings_t;

/* GLOBALS ---------------------------------------------------------------- */

static gpioReport_t   gReport[PISCOPE_MAX_REPORTS_PER_READ];

static int            gDebugLevel = 0;

static uint32_t       gTimeSlotMicros;
static uint32_t       gInputUpdateHz  = 40;
static uint32_t       gOutputUpdateHz = 20;
static int64_t        gRefreshTicks;
static int32_t        gPlaySpeed = PISCOPE_DEF_SPEED_IDX;

static struct timeval gTimeOrigin;
static int64_t        gTickOrigin;

static int64_t        g1Tick;
static int64_t        g2Tick;
static int64_t        gGoldTick;
static int64_t        gBlueTick;

static uint32_t       gHilitGpios;

static int            gTriggerFired;
static int            gTriggerCount;

static int64_t        gSampleTick[PISCOPE_SAMPLES];
static uint32_t       gSampleLevel[PISCOPE_SAMPLES];

static piscopeState_t gInputState  = piscope_initialise;
static piscopeState_t gOutputState = piscope_initialise;

static int            gBufReadPos;

static int            gBufWritePos;

static int            gBufSamples;

static int            gGpioTempDisplay[PISCOPE_GPIOS];

static int            gCoscWidth  = 400;
static int            gCoscHeight = 300;
static int            gChlegWidth;
static int            gChlegHeight;
static int            gCvlegWidth;
static int            gCvlegHeight;
static int            gCsampWidth;
static int            gCsampHeight;
static int            gCmodeWidth;
static int            gCmodeHeight;

static int            gPigSocket = -1;
static int            gPigHandle = -1;
static int            gPigNotify = -1;

static int            gPigConnected = 0;

static int            gRPiRevision  = 0;

static int64_t        gViewStartTick;
static int64_t        gViewCentreTick;
static int64_t        gViewEndTick;

static int64_t        gFirstReportTick;
static int64_t        gLastReportTick;

static int64_t        gViewTicks;

static int            gViewStartSample;
static int            gViewEndSample;

static int            gTriggerNum = 0;

static int            gZoomLevel = PISCOPE_DEFAULT_ZOOM_LEVEL;

static uint32_t       gDeciMicroPerPix = 20000;

static uint32_t       gZoomDeciMicroPerPix[]=
{
           1,         2,         5,         10,         20,         50,
         100,       200,       500,       1000,       2000,       5000,
       10000,     20000,     50000,     100000,     200000,     500000,
     1000000,   2000000,   5000000,   10000000,   20000000,   50000000,
   100000000, 200000000, 500000000, 1000000000, 2000000000, 4000000000u,
};

static GtkWidget        *gMain;

static GtkWidget        *gMainCbuf;
static GtkWidget        *gMainCosc;
static GtkWidget        *gMainChleg;
static GtkWidget        *gMainCvleg;
static GtkWidget        *gMainCmode;

static GtkWidget        *gMainLtime;
static GtkWidget        *gMainLmode;
static GtkWidget        *gMainLtrigs;
static GtkWidget        *gMainLgold;
static GtkWidget        *gMainLblue;

static GtkWidget        *gMainTBpause;
static GtkWidget        *gMainTBplay;
static GtkWidget        *gMainTBlive;

static GtkWidget        *gCmdsDialog;
static GtkWidget        *gGpioDialog;
static GtkWidget        *gTrigDialog;
static GtkWidget        *gTrgsDialog;

static GtkWidget        *gTrigLabel;
static GtkWidget        *gTrgsSamples;

static GtkWidget        *gMainTBconnect;

static GtkWidget        *gCmdsPlayspeed;
static GtkWidget        *gCmdsPigpioAddr;
static GtkWidget        *gCmdsPigpioPort;

static char             *gTrigTypeText[]=
{
   "-", "Low", "High", "Edge", "Falling", "Rising",
};

static char              *gTrigWhenText[]=
{
   "count", "sample from", "sample around", "sample to",
};

static char              *gTrigSamplesText[]=
{
   "100", "200", "500", "1000", "2000", "5000", "10000", "20000", "50000",
};

static int                gTrigSamples;

static GtkComboBoxText   *gTrigCombo[PISCOPE_GPIOS];

static piscopeGpioUsage_t gGpioUsage[][PISCOPE_GPIOS]=
{
   {
      {1,"SDA"  },{1,"SCL"  },{0,NULL  },{0,NULL  },{1,NULL  },{0,NULL  },
      {0,NULL   },{1,"CE1"  },{1,"CE0" },{1,"MISO"},{1,"MOSI"},{1,"SCLK"},
      {0,NULL   },{0,NULL   },{1,"TXD" },{1,"RXD" },{0,NULL  },{1,NULL  },
      {1,NULL   },{0,NULL   },{0,NULL  },{1,NULL  },{1,NULL  },{1,NULL  },
      {1,NULL   },{1,NULL   },{0,NULL  },{0,NULL  },{0,NULL  },{0,NULL  },
      {0,NULL   },{0,NULL   },
   },
   {
      {0,NULL   },{0,NULL   },{1,"SDA" },{1,"SCL" },{1,NULL  },{0,NULL  },
      {0,NULL   },{1,"CE1"  },{1,"CE0" },{1,"MISO"},{1,"MOSI"},{1,"SCLK"},
      {0,NULL   },{0,NULL   },{1,"TXD" },{1,"RXD" },{0,NULL  },{1,NULL  },
      {1,NULL   },{0,NULL   },{0,NULL  },{0,NULL  },{1,NULL  },{1,NULL  },
      {1,NULL   },{1,NULL   },{0,NULL  },{1,NULL  },{1,NULL  },{1,NULL  },
      {1,NULL   },{1,NULL   },
   },
   {
      {0,"ID_SD"},{0,"ID_SC"},{1,"SDA" },{1,"SCL" },{1,NULL  },{1,NULL  },
      {1,NULL   },{1,"CE1"  },{1,"CE0" },{1,"MISO"},{1,"MOSI"},{1,"SCLK"},
      {1,NULL   },{1,NULL   },{1,"TXD" },{1,"RXD" },{1,"ce2" },{1,"ce1" },
      {1,"ce0"  },{1,"miso" },{1,"mosi"},{1,"sclk"},{1,NULL  },{1,NULL  },
      {1,NULL   },{1,NULL   },{1,NULL  },{1,NULL  },{0,NULL  },{0,NULL  },
      {0,NULL   },{0,NULL   },
   },
};

static piscopeTrigInfo_t gTrigInfo[PISCOPE_TRIGGERS];

static piscopeGpioInfo_t gGpioInfo[PISCOPE_GPIOS];

static int               gDisplayedGpios = PISCOPE_GPIOS;

static piscopeMode_t     gMode = piscope_live;

static cairo_surface_t  *gCoscSurface  = NULL;
static cairo_surface_t  *gChlegSurface = NULL;
static cairo_surface_t  *gCvlegSurface = NULL;
static cairo_surface_t  *gCsampSurface = NULL;
static cairo_surface_t  *gCmodeSurface    = NULL;

static cairo_t          *gCoscCairo = NULL;

static piscopeSettings_t gSettings;

gboolean main_osc_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
);

void main_util_setWindowTitle();

/* FUNCTIONS -------------------------------------------------------------- */


/* UTIL ------------------------------------------------------------------- */

static void util_hlegConfigure(GtkWidget *widget)
{
   int i, margin, len, adj, div, ticks;
   int64_t micros;
   cairo_text_extents_t te;
   cairo_t *cr;
   char *units;
   char digBuf[8];
   char strBuf[32];

   if (gChlegSurface) cairo_surface_destroy(gChlegSurface);

   gChlegWidth  = gtk_widget_get_allocated_width(widget);
   gChlegHeight = gtk_widget_get_allocated_height(widget);

   gChlegSurface = gdk_window_create_similar_surface
   (
      gtk_widget_get_window(widget),
      CAIRO_CONTENT_COLOR,
      gChlegWidth,
      gChlegHeight
   );

   /* Initialize the surface to white */

   cr = cairo_create(gChlegSurface);

   cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

   cairo_paint(cr);

   /* a tick every 10 pixels with equal margins */

   i = gChlegWidth / 10;

   margin = gChlegWidth - (i * 10);

   if (margin < 4) margin +=10;

   margin /= 2;

   cairo_set_line_width(cr, 0.2);

   cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);

   ticks = 0;

   for (i=margin; i<gChlegWidth; i+=10)
   {
      if (ticks%10) len = 5; else len = 10;

      cairo_move_to(cr, i, gChlegHeight-len);

      cairo_line_to(cr, i, gChlegHeight);

      ++ticks;
   }

   cairo_stroke(cr);

   cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);

   micros = 10 * (int64_t)gDeciMicroPerPix;

   if      (micros >=    1000000) {div=   1000000; units="s" ;}
   else if (micros >=       1000) {div=      1000; units="ms";}
   else                           {div=         1; units="us";}

   ticks = 0;

   for (i=margin; i<gChlegWidth; i+=100)
   {
      micros = ticks * 10 * (int64_t)gDeciMicroPerPix;

      sprintf(digBuf, "%d", (int)(micros/div));
      cairo_text_extents(cr, digBuf, &te);
      adj = (te.width / 2) + 2;

      sprintf(strBuf, "%s %s", digBuf, units);

      cairo_move_to(cr, i-adj, gChlegHeight-10);
      cairo_show_text(cr, strBuf);

      ++ticks;
   }

   cairo_destroy(cr);
}

static void util_vlegConfigure(GtkWidget *widget)
{
   cairo_t *cr;
   int i;
   char buf[16];

   if (!gtk_widget_get_window(widget)) return;

   if (gCvlegSurface) cairo_surface_destroy(gCvlegSurface);

   gCvlegWidth  = gtk_widget_get_allocated_width(widget);
   gCvlegHeight = gtk_widget_get_allocated_height(widget);

   gCvlegSurface = gdk_window_create_similar_surface
   (
      gtk_widget_get_window(widget),
      CAIRO_CONTENT_COLOR,
      gCvlegWidth,
      gCvlegHeight
   );

   /* Initialize the surface to white */

   cr = cairo_create(gCvlegSurface);

   cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

   cairo_paint(cr);

   gHilitGpios = 0;

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      if (gGpioInfo[i].display)
      {
         if (gGpioInfo[i].hilit)
         {
            gHilitGpios |= (1<<i);

            cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);

            cairo_rectangle(cr, 0, gGpioInfo[i].y_high-2,
               gCvlegWidth, gGpioInfo[i].y_low - gGpioInfo[i].y_high + 3);

            cairo_fill(cr);

            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
         }
         else cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);

         cairo_move_to(cr, 2, gGpioInfo[i].y_low-2);

         if (gGpioInfo[i].name) sprintf(buf, "%2d %s", i, gGpioInfo[i].name);
         else                   sprintf(buf, "%2d", i);

         cairo_show_text(cr, buf);
      }
   }

   cairo_destroy(cr);
}

static void util_zoom_def_clicked(void)
{
   if (gZoomLevel != PISCOPE_DEFAULT_ZOOM_LEVEL)
   {
      gZoomLevel = PISCOPE_DEFAULT_ZOOM_LEVEL;

      gDeciMicroPerPix = gZoomDeciMicroPerPix[gZoomLevel];

      gViewTicks = (((int64_t)gCoscWidth * gDeciMicroPerPix)/10);

      util_hlegConfigure(gMainChleg);

      gtk_widget_queue_draw(gMainChleg);
   }
}

static void util_trig_activate(int trigger)
{
   int i;
   char label[16];

   if (gTriggerNum) return;

   gTriggerNum = trigger;

   /* disable trigger */

   gTrigInfo[gTriggerNum-1].enabled = 0;

   gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(gTrigInfo[gTriggerNum-1].onW), 0);

   sprintf(label, "Trigger #%d", trigger);

   gtk_label_set_text(GTK_LABEL(gTrigLabel), label);

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gtk_combo_box_set_active(
         GTK_COMBO_BOX(gTrigCombo[i]), gTrigInfo[gTriggerNum-1].type[i]);
   }

   gtk_widget_show(gTrigDialog);
}

static void util_mode_display(void)
{
   cairo_t *cr;

   cr = cairo_create(gCmodeSurface);

   switch(gMode)
   {
      case piscope_live:
         cairo_set_source_rgb(cr, 0.2, 0.6, 0.1);
         break;

      case piscope_play:
         cairo_set_source_rgb(cr, 1.0, 0.7, 0.2);
         break;

      case piscope_pause:
         cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
         break;
   }

   cairo_paint(cr);

   cairo_destroy(cr);

   gtk_widget_queue_draw(gMainCmode);
}

static void util_labelText(GtkWidget *label, char *text)
{
   gtk_label_set_text((GtkLabel*)label, text);
}

static const char *util_playSpeedStr(int speed)
{
   static char buf[16];

   if (speed <= PISCOPE_DEF_SPEED_IDX)
      sprintf(buf, "%dX", (1<<(PISCOPE_DEF_SPEED_IDX - speed)));
   else
      sprintf(buf, "1/%d", (1<<(speed - PISCOPE_DEF_SPEED_IDX)));

   return buf;
}

static void util_calcGpioY(void)
{
   int pix, margin, i, y;

   pix = gCoscHeight / gDisplayedGpios;

   margin = gCoscHeight - (pix * gDisplayedGpios);

   while (margin < 4)
   {
      --pix;
      margin += gDisplayedGpios;
   }

   y = pix + (margin/2);

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      if (gGpioInfo[i].display)
      {
         gGpioInfo[i].y_high = y-pix+2;
         gGpioInfo[i].y_low  = y-2;
         gGpioInfo[i].y_tick = y;
         y += pix;
      }
   }

   util_vlegConfigure(gMainCvleg);

   gtk_widget_queue_draw(gMainCvleg);

   util_hlegConfigure(gMainChleg);
   gtk_widget_queue_draw(gMainChleg);
}

static void util_setViewMode(int mode)
{
   char buf[32];

   gMode = mode;

   util_mode_display();

   switch(mode)
   {
      case piscope_pause:
         util_labelText(gMainLmode, "PAUSE");
         break;

      case piscope_play:
         sprintf(buf, "PLAY %s", util_playSpeedStr(gPlaySpeed));
         util_labelText(gMainLmode, buf);
         break;

      case piscope_live:
         util_labelText(gMainLmode, "LIVE");
         break;
   }
}

static int util_popupMessage(int type, int buttons, const gchar * format, ...)
{
   int status;
   GtkWidget *dialog;
   va_list args;
   char str[256];

   /*
   type

   GTK_MESSAGE_INFO,
   GTK_MESSAGE_WARNING,
   GTK_MESSAGE_QUESTION,
   GTK_MESSAGE_ERROR

   buttons

   GTK_BUTTONS_NONE,
   GTK_BUTTONS_OK,
   GTK_BUTTONS_CLOSE,
   GTK_BUTTONS_CANCEL,
   GTK_BUTTONS_YES_NO,
   GTK_BUTTONS_OK_CANCEL

  */

   va_start(args, format);

   vsnprintf(str, sizeof(str), format, args);

   va_end(args);

   dialog = gtk_message_dialog_new
   (
      GTK_WINDOW(gMain),
      GTK_DIALOG_DESTROY_WITH_PARENT,
      type,
      buttons,
      "%s", str
   );

   status = gtk_dialog_run(GTK_DIALOG(dialog));

   gtk_widget_destroy(dialog);

   return status;
}

static char *util_timeStamp(int64_t *tick, int decimals, int blue)
{
   static struct timeval last;
   static char buf[64];
   static int usecs[]={1000000, 100000, 10000, 1000, 100, 10, 1};

   struct tm tmp;
   struct timeval now, offsetTime;
   int64_t offsetTick;
   char blueBuf[64];

   offsetTick = *tick - gTickOrigin;

   offsetTime.tv_sec  = offsetTick / PISCOPE_MILLION;
   offsetTime.tv_usec = offsetTick % PISCOPE_MILLION;

   if (blue)
   {
      if (decimals)
         sprintf
         (
            blueBuf,
            "%d.%0*d ",
            (int)offsetTime.tv_sec,
            decimals,
            (int)offsetTime.tv_usec/usecs[decimals]
         );
      else
         sprintf
         (
            blueBuf,
            "%d ",
            (int)offsetTime.tv_sec
         );

      gtk_label_set_text((GtkLabel*)gMainLblue, blueBuf);
   }

   timeradd(&gTimeOrigin, &offsetTime, &now);

   if (now.tv_sec != last.tv_sec)
   {
      /* only reformat date/time once per second */

      last.tv_sec = now.tv_sec;
      localtime_r(&now.tv_sec, &tmp);
      strftime(buf, sizeof(buf), "%F %T", &tmp);
   }

   if (decimals)
      sprintf(buf+19, ".%0*d ", decimals, (int)now.tv_usec/usecs[decimals]);
   else buf[19] = 0;

   return buf;
}

/* PIGPIO ----------------------------------------------------------------- */

static int pigpioCommand(int fd, int command, int p1, int p2)
{
   cmdCmd_t cmd;

   if (fd < 0) return piscope_bad_socket;

   cmd.cmd = command;
   cmd.p1  = p1;
   cmd.p2  = p2;
   cmd.res = 0;

   if (send(fd, &cmd, sizeof(cmdCmd_t), 0) != sizeof(cmdCmd_t))
      return piscope_bad_send;

   if (recv(fd, &cmd, sizeof(cmdCmd_t), MSG_WAITALL) != sizeof(cmdCmd_t))
      return piscope_bad_recv;

   return cmd.res;
}

static void pigpioSetAddr(void)
{
   char * portStr, * addrStr;
   char buf[16];

   portStr = getenv(PI_ENVPORT);

   if ((!portStr) || (!strlen(portStr)))
   {
      sprintf(buf, "%d", gSettings.port);
      portStr = buf;
   }

   addrStr = getenv(PI_ENVADDR);

   if ((!addrStr) || (!strlen(addrStr)))
   {
      addrStr=gSettings.serverAddress;
   }

   gtk_entry_set_text(GTK_ENTRY(gCmdsPigpioAddr), addrStr);

   gtk_entry_set_text(GTK_ENTRY(gCmdsPigpioPort), portStr);
}

static void pigpioLoadSettings(void)
{
   GKeyFile *cfg;
   char *file, buf[50];
   gint *tempList;
   gboolean loaded;
   int i, j;
   gsize len;

   cfg = g_key_file_new();
   file = g_build_filename(g_get_user_config_dir(), SETTINGS_FILE_NAME, NULL);

   loaded = g_key_file_load_from_file(cfg, file, G_KEY_FILE_NONE, NULL);

   g_free(gSettings.serverAddress);
   g_free(gSettings.activeGPIOs);
   gSettings.serverAddress=NULL;
   gSettings.activeGPIOs=NULL;
   gSettings.activeGPIOCount=0;

   if(loaded)
   {
      gSettings.serverAddress = g_key_file_get_string(cfg, SETTINGS_GROUP, SETTINGS_SERVER_ADDRESS, NULL);
      gSettings.activeGPIOs = g_key_file_get_integer_list (cfg, SETTINGS_GROUP, SETTINGS_ACTIVE_GPIOS, &gSettings.activeGPIOCount, NULL);
      gSettings.port = g_key_file_get_integer(cfg, SETTINGS_GROUP, SETTINGS_SERVER_PORT, NULL);
      gSettings.triggerSamples = g_key_file_get_integer(cfg, SETTINGS_GROUP, SETTINGS_TRIGGER_SAMPLES, NULL);
      for(i=0; i<PISCOPE_TRIGGERS; i++)
         {
            sprintf(buf, SETTINGS_TRIGGER_ENABLED, i+1);
            gSettings.triggers[i].enabled = g_key_file_get_boolean(cfg, SETTINGS_GROUP, buf, NULL);
            sprintf(buf, SETTINGS_TRIGGER_ACTION, i+1);
            gSettings.triggers[i].action = g_key_file_get_integer(cfg, SETTINGS_GROUP, buf, NULL);
            sprintf(buf, SETTINGS_TRIGGER_GPIO_TYPES, i+1);
            tempList = g_key_file_get_integer_list(cfg, SETTINGS_GROUP, buf, &len, NULL);
            if(tempList)
               {
                  for(j=0; j<len && j<PISCOPE_GPIOS; j++)
                     {
                        gSettings.triggers[i].gpiotypes[j] = tempList[j];
                     }
               }
            g_free(tempList);
         }
   }

   if(!gSettings.serverAddress)
   {
      gSettings.serverAddress = g_malloc(sizeof(PI_DEFAULT_SERVER_ADDRESS));
      strcpy (gSettings.serverAddress, PI_DEFAULT_SERVER_ADDRESS);
      gSettings.port = PI_DEFAULT_SOCKET_PORT;
   }

   g_free(file);
   g_key_file_free(cfg);
}

static void pigpioSaveSettings(void)
{
   GKeyFile * cfg;
   char * file, buf[50];
   int i;

   cfg = g_key_file_new();
   file = g_build_filename(g_get_user_config_dir(), SETTINGS_FILE_NAME, NULL);

   g_key_file_set_string(cfg, SETTINGS_GROUP, SETTINGS_SERVER_ADDRESS, gSettings.serverAddress);
   g_key_file_set_integer(cfg, SETTINGS_GROUP, SETTINGS_SERVER_PORT, gSettings.port);
   if(gSettings.activeGPIOs)
      g_key_file_set_integer_list(cfg, SETTINGS_GROUP, SETTINGS_ACTIVE_GPIOS, gSettings.activeGPIOs, gSettings.activeGPIOCount);

   g_key_file_set_integer(cfg, SETTINGS_GROUP, SETTINGS_TRIGGER_SAMPLES, gSettings.triggerSamples);
   for(i=0; i<PISCOPE_TRIGGERS; i++)
      {
         sprintf(buf, SETTINGS_TRIGGER_ENABLED, i+1);
         g_key_file_set_boolean(cfg, SETTINGS_GROUP, buf, gSettings.triggers[i].enabled);
         sprintf(buf, SETTINGS_TRIGGER_ACTION, i+1);
         g_key_file_set_integer(cfg, SETTINGS_GROUP, buf, gSettings.triggers[i].action);
         sprintf(buf, SETTINGS_TRIGGER_GPIO_TYPES, i+1);
         g_key_file_set_integer_list(cfg, SETTINGS_GROUP, buf, gSettings.triggers[i].gpiotypes, PISCOPE_GPIOS);
      }
   g_key_file_save_to_file(cfg, file, NULL);

   g_free(file);
   g_key_file_free(cfg);
}

static int cmpfunc(const void * a, const void * b) {
   return ( *(int*)a - *(int*)b );
}

static void pigpioSetGpios(void)
{
   int i, hwver;
   uint32_t notifyBits = 0;

   gDisplayedGpios = 0;

   hwver = pigpioCommand(gPigSocket, PI_CMD_HWVER, 0, 0);

   if      (hwver <  0) gRPiRevision = 0;
   else if (hwver <  4) gRPiRevision = 1;
   else if (hwver < 16) gRPiRevision = 2;
   else                 gRPiRevision = 3;

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gGpioInfo[i].display = 0;

      switch (gRPiRevision)
      {
         case 1:
         case 2:
         case 3:
            gGpioInfo[i].name = gGpioUsage[gRPiRevision-1][i].name;

            if (gGpioUsage[gRPiRevision-1][i].usable && (!gSettings.activeGPIOs ||
                  bsearch(&i, gSettings.activeGPIOs, gSettings.activeGPIOCount, sizeof (int), cmpfunc)))
               gGpioInfo[i].display = 1;

            break;

         default:
            gGpioInfo[i].name = NULL;
            gGpioInfo[i].display = 1;
      }

      if (gGpioInfo[i].display)
      {
         gDisplayedGpios++;
         notifyBits |= (1<<i);
      }

      gtk_toggle_button_set_active(
         gGpioInfo[i].button, gGpioInfo[i].display);
   }

   pigpioCommand(gPigSocket, PI_CMD_NB, gPigHandle, notifyBits);
}

static void util_setTriggerGPIOTypes(int triggerNum)
{
   int i, v;
   uint32_t levelMask;
   uint32_t changedMask;
   uint32_t levelValue;

   levelMask    = 0;
   levelValue   = 0;
   changedMask  = 0;

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      v = gSettings.triggers[triggerNum].gpiotypes[i];
      gTrigInfo[triggerNum].type[i] = v;

      switch (v)
      {
         case piscope_dont_care:
            break;

         case piscope_low:
            levelMask |= (1<<i);
            break;

         case piscope_high:
            levelMask  |= (1<<i);
            levelValue |= (1<<i);
            break;

         case piscope_edge:
            changedMask |= (1<<i);
            break;

         case piscope_falling:
            levelMask   |= (1<<i);
            changedMask |= (1<<i);
            break;

         case piscope_rising:
            levelMask   |= (1<<i);
            levelValue  |= (1<<i);
            changedMask |= (1<<i);
            break;
      }
   }

   gTrigInfo[triggerNum].levelMask = levelMask;
   gTrigInfo[triggerNum].levelValue = levelValue;
   gTrigInfo[triggerNum].changedMask = changedMask;
}

static void pigpioSetTriggers(void)
{
   int i;

   sscanf(gTrigSamplesText[gSettings.triggerSamples], "%d", &gTrigSamples);
   gtk_combo_box_set_active(GTK_COMBO_BOX(gTrgsSamples),  gSettings.triggerSamples);

   for(i=0; i<PISCOPE_TRIGGERS; i++)
   {
      gTrigInfo[i].enabled = gSettings.triggers[i].enabled;
      gTrigInfo[i].when = gSettings.triggers[i].action;
      util_setTriggerGPIOTypes(i);

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gTrigInfo[i].onW), gTrigInfo[i].enabled);
      gtk_combo_box_set_active(GTK_COMBO_BOX(gTrigInfo[i].whenW), gTrigInfo[i].when);
   }
}

static void pigpioSetState(void)
{
   if (gPigConnected)
   {
      gtk_toggle_tool_button_set_active(
         GTK_TOGGLE_TOOL_BUTTON(gMainTBconnect), TRUE);

      gtk_toggle_tool_button_set_active(
         GTK_TOGGLE_TOOL_BUTTON(gMainTBlive), TRUE);
   }
   else
   {
      gtk_toggle_tool_button_set_active(
         GTK_TOGGLE_TOOL_BUTTON(gMainTBconnect), FALSE);

      gtk_toggle_tool_button_set_active(
         GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);
   }

   main_util_setWindowTitle();
   
   util_zoom_def_clicked();
}

static int pigpioOpenSocket(void)
{
   int sock, err;
   struct addrinfo hints, *res, *rp;
   const gchar *addrStr, *portStr;

   addrStr = gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr));

   portStr = gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioPort));

   memset (&hints, 0, sizeof (hints));

   hints.ai_family   = PF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags   |= AI_CANONNAME;

   err = getaddrinfo (addrStr, portStr, &hints, &res);

   if (err) return piscope_bad_getaddrinfo;

   for (rp=res; rp!=NULL; rp=rp->ai_next)
   {
      sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

      if (sock == -1) continue;

      if (connect(sock, rp->ai_addr, rp->ai_addrlen) != -1) break;
   }

   freeaddrinfo(res);

   if (rp == NULL) return piscope_bad_connect;

   return sock;
}

static int pigpioOpenNotifications(void)
{
   int r;

   gPigNotify = pigpioOpenSocket();

   if (gPigNotify < 0) return piscope_bad_nsock;

   r = pigpioCommand(gPigNotify, PI_CMD_NOIB, 0, 0);

   if (r < 0) return piscope_bad_noib;

   gPigHandle = r;

   return 0;
}

static void pigpioConnect(void)
{
   char msg[256];

   if (!gPigConnected)
   {
      gBufWritePos   = -1;
      gBufReadPos    =  0;
      gBufSamples    =  0;

      gPigSocket = pigpioOpenSocket();

      if (gPigSocket >= 0)
      {
         gPigConnected = 1;

         pigpioOpenNotifications();

         gInputState = piscope_initialise;
      }
      else
      {
         gPigConnected = 0;

         snprintf(msg, sizeof(msg),
            "Can't connect to pigpio at %s.\nDid you sudo pigpiod?\nIf you are on a remote client, have you set the server address and port?",
             gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr)));
         
         util_popupMessage(GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, msg);
      }

      pigpioSetGpios();

      pigpioSetTriggers();

      pigpioSetState();

      util_calcGpioY();
   }
}

static void pigpioDisconnect(void)
{
   if (gPigConnected)
   {
      gInputState = piscope_dormant;

      gPigConnected = 0;

      if (gPigSocket >= 0)
      {
         if (gPigHandle >= 0)
         {
            pigpioCommand(gPigSocket, PI_CMD_NC, gPigHandle, 0);
            gPigHandle = -1;
         }

         close(gPigSocket);
         gPigSocket = -1;
      }

      if (gPigNotify >= 0)
      {
         close(gPigNotify);
         gPigNotify = -1;
      }

      pigpioSetGpios();

      pigpioSetState();

      util_calcGpioY();
   }
}

/* CMDS ------------------------------------------------------------------- */

void cmds_clear_triggers_clicked(GtkButton * button, gpointer user_data)
{
   int i;

   for (i=0; i<PISCOPE_TRIGGERS; i++) gTrigInfo[i].count = 0;
}

void cmds_close_clicked(GtkButton * button, gpointer user_data)
{
   const char *serverAddress;
   char msg[128];
   
   gtk_widget_hide(gCmdsDialog);

   if (gSettings.serverAddress)
   {
      g_free(gSettings.serverAddress);
      gSettings.serverAddress=NULL;
   }
   serverAddress = gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr));
   gSettings.serverAddress = g_malloc(strlen(serverAddress)+1);
   strcpy(gSettings.serverAddress, serverAddress);
   gSettings.port = strtol(gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioPort)), NULL, 10);
   
   if (!gPigConnected)
   {
      snprintf(msg, sizeof(msg), "Connect to pigpio at %s", serverAddress);
      gtk_widget_set_tooltip_text(GTK_WIDGET(gMainTBconnect), msg);
   }

   pigpioSaveSettings();
}

void cmds_emptybuf_clicked(GtkButton * button, gpointer user_data)
{
   if (util_popupMessage(GTK_MESSAGE_QUESTION,
                    GTK_BUTTONS_YES_NO,
                    "Clear all samples?") ==  GTK_RESPONSE_YES)
   {
      gBufWritePos = -1;
      gBufReadPos  =  0;
      gBufSamples  =  0;

      gBlueTick = 0;
      gGoldTick = 0;
      g1Tick = 0;
      g2Tick = 0;

      gTickOrigin = 0;

      main_osc_configure_event(gMainCosc, NULL, NULL);
   }
}

void cmds_playspeed_changed(GtkComboBox *widget, gpointer user_data)
{
   gPlaySpeed = gtk_combo_box_get_active(widget);

   if (gMode == piscope_play) util_setViewMode(piscope_play);
}

/* FILE ------------------------------------------------------------------- */

static int file_VCDsymbol(int bit)
{
   if (bit < 26) return ('A' + bit);
   else          return ('a' + bit - 26);
}

static int file_load(char *filename)
{
   uint32_t level;
   int64_t tick64;
   int index;
   int err;
   FILE * in;
   time_t time;
   struct tm cal;
   char buf[512];

   err = 1;

   in = fopen(filename, "r");

   if (in == NULL) return errno;

   if (fgets(buf, 32, in))
   {
      if (strcmp(buf, "#piscope\n") == 0)
      {
         if (fgets(buf, 64, in))
         {
            if (strncmp(buf, "#date ", 6) == 0)
            {
               sscanf(buf, "#date %d-%d-%d %d:%d:%d",
                  &cal.tm_year, &cal.tm_mon, &cal.tm_mday,
                  &cal.tm_hour, &cal.tm_min, &cal.tm_sec);

               /* need tm_mon in 0..11 */
               --cal.tm_mon;

               /* need tm_year from 1900 */
               cal.tm_year -= 1900;

               time = mktime(&cal);

               gTimeOrigin.tv_sec  = time;
               gTimeOrigin.tv_usec = 0;

               gTickOrigin = 0;
               gGoldTick   = 0;

               err = 0;
               index = 0;

               while (fscanf(in, "%Ld %08X\n",
                        (long long int *) &tick64, &level) == 2)
               {
                  if (index < PISCOPE_SAMPLES)
                  {
                     gSampleTick[index]  = tick64;
                     gSampleLevel[index] = level;
                     ++index;
                  }
               }
               gBufSamples = index;
               gBufReadPos = 0;
               gBufWritePos = index -1 ;
            }
         }
      }
   }

   if (err)
   {
      util_popupMessage
      (
         GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
         "%s\nis not a legal .piscope file",
         filename
      );
   }

   fclose(in);

   return 0;
}

static int file_save(int filetype, char *filename, int selection)
{
   int b, r, p, v;
   uint32_t lastLevel, changed, level;
   FILE * out;

   out = fopen(filename, "w");

   if (out == NULL) return errno;

   if (filetype == piscope_vcd)
   {
      fprintf(out, "$date %s $end\n", util_timeStamp(&gTickOrigin, 0, 0));
      fprintf(out, "$version piscope V1 $end\n");
      fprintf(out, "$timescale 1 us $end\n");
      fprintf(out, "$scope module top $end\n");

      for (b=0; b<32; b++)
         fprintf(out, "$var wire 1 %c %d $end\n", file_VCDsymbol(b), b);

      fprintf(out, "$upscope $end\n");
      fprintf(out, "$enddefinitions $end\n");
   }
   else
   {
      fprintf(out, "#piscope\n");
      fprintf(out, "#date %s\n", util_timeStamp(&gTickOrigin, 0, 0));
   }

   p = gBufReadPos;

   lastLevel = ~gSampleLevel[p];

   for (r=0; r<gBufSamples; r++)
   {
      if (!selection ||
        ((gSampleTick[p] >= g1Tick) && (gSampleTick[p] <= g2Tick)))
      {
         if (filetype == piscope_vcd)
         {
            fprintf(out, "#%Ld\n",
               (long long int)(gSampleTick[p]-gTickOrigin));

            level = gSampleLevel[p];

            changed = level ^ lastLevel;

            for (b=0; b<32; b++)
            {
               if (changed & (1<<b))
               {
                  if (level & (1<<b)) v='1'; else v='0';

                  fprintf(out, "%c%c\n", v, file_VCDsymbol(b));
               }
            }

            lastLevel = gSampleLevel[p];
         }
         else
         {
            fprintf
            (
               out,
               "%Ld %08X\n",
               (long long int)(gSampleTick[p]-gTickOrigin),
               gSampleLevel[p]
            );
         }
      }

      if (++p >= PISCOPE_SAMPLES) p = 0;
   }

   fclose(out);

   return 0;
}

/* GPIO ------------------------------------------------------------------- */

void gpio_clear_all(GtkButton * button, gpointer user_data)
{
   int i;
   /* clear states */
   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gtk_toggle_button_set_active(gGpioInfo[i].button, FALSE);
   }
}

void gpio_set_all(GtkButton * button, gpointer user_data)
{
   int i;
   /* set states */
   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gtk_toggle_button_set_active(gGpioInfo[i].button, TRUE);
   }
}

void gpio_invert_all(GtkButton * button, gpointer user_data)
{
   int i;
   /* invert states */
   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      if (gtk_toggle_button_get_active(gGpioInfo[i].button))
         gtk_toggle_button_set_active(gGpioInfo[i].button, FALSE);
      else
         gtk_toggle_button_set_active(gGpioInfo[i].button, TRUE);      
   }
}

void gpio_apply_clicked(GtkButton * button, gpointer user_data)
{
   int i;
   uint32_t notifyBits = 0;

   gtk_widget_hide(gGpioDialog);

   /* get and set states */

   g_free(gSettings.activeGPIOs);
   gSettings.activeGPIOCount = gDisplayedGpios = 0;
   gSettings.activeGPIOs = g_malloc(PISCOPE_GPIOS * sizeof(gint));

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gGpioInfo[i].display =
         gtk_toggle_button_get_active(gGpioInfo[i].button);

      if (gGpioInfo[i].display)
      {
         gSettings.activeGPIOs[gSettings.activeGPIOCount++] = i;
         gDisplayedGpios++;

         notifyBits |= (1<<i);
      }
   }

   if (gDisplayedGpios == 0)
   {
      /* not worth the hassle of allowing zero gpios,
         arbitrarily display gpio#0 */

      gDisplayedGpios      = 1;
      gGpioInfo[0].display = 1;
      gSettings.activeGPIOs[0] = 0;
      gSettings.activeGPIOCount = 1;
   }

   pigpioCommand(gPigSocket, PI_CMD_NB, gPigHandle, notifyBits);

   util_calcGpioY();

   pigpioSaveSettings();
}

void gpio_cancel_clicked(GtkButton * button, gpointer user_data)
{
   int i;

   gtk_widget_hide(gGpioDialog);

   /* set the gpio states to those saved earlier */

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gtk_toggle_button_set_active(gGpioInfo[i].button, gGpioTempDisplay[i]);
   }
}

/* TRGS ------------------------------------------------------------------- */

static void trgs_reset(void)
{
   int i;

   gTriggerFired = 0;
   gTriggerCount = 0;

   for (i=0; i<PISCOPE_TRIGGERS; i++) gTrigInfo[i].fired = 0;
}

static void trgs_lab_str(int trig)
{
   int i;
   char *trigTypeStr= "-01EFR";
   char buf[PISCOPE_GPIOS+1];

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      buf[i] = trigTypeStr[gTrigInfo[trig].type[i]];
   }

   buf[PISCOPE_GPIOS] = 0;

   util_labelText(gTrigInfo[trig].labelW, buf);
}

static void trgs_when_changed(GtkComboBox *widget, int trig)
{
   gTrigInfo[trig-1].when = gtk_combo_box_get_active(widget);
}

void trgs_when1_changed(GtkComboBox *widget, gpointer user_data)
{
   trgs_when_changed(widget, 1);
}

void trgs_when2_changed(GtkComboBox *widget, gpointer user_data)
{
   trgs_when_changed(widget, 2);
}

void trgs_when3_changed(GtkComboBox *widget, gpointer user_data)
{
   trgs_when_changed(widget, 3);
}

void trgs_when4_changed(GtkComboBox *widget, gpointer user_data)
{
   trgs_when_changed(widget, 4);
}

void trgs_samples_changed(GtkComboBox *widget, gpointer user_data)
{
   int i;

   i = gtk_combo_box_get_active(widget);

   sscanf(gTrigSamplesText[i], "%d", &gTrigSamples);
}

static void trgs_on_toggled(GtkToggleButton *button, int trig)
{
   int on;

   on = gtk_toggle_button_get_active(button);

   if (gTrigInfo[trig-1].levelMask | gTrigInfo[trig-1].changedMask)
   {
      if (on)
      {
         gTrigInfo[trig-1].enabled = 1;
      }
      else
      {
         gTrigInfo[trig-1].enabled = 0;
      }
   }
   else
   {
      if (on)
      {
         /* illegal trigger, force off */
         gtk_toggle_button_set_active(button, 0);
      }
   }
}

void trgs_on1_toggled(GtkToggleButton * button, gpointer user_data)
{
   trgs_on_toggled(button, 1);
}

void trgs_on2_toggled(GtkToggleButton * button, gpointer user_data)
{
   trgs_on_toggled(button, 2);
}

void trgs_on3_toggled(GtkToggleButton * button, gpointer user_data)
{
   trgs_on_toggled(button, 3);
}

void trgs_on4_toggled(GtkToggleButton * button, gpointer user_data)
{
   trgs_on_toggled(button, 4);
}

void trgs_edit1_clicked(GtkButton * button, gpointer user_data)
{
   util_trig_activate(1);
}

void trgs_edit2_clicked(GtkButton * button, gpointer user_data)
{
   util_trig_activate(2);
}

void trgs_edit3_clicked(GtkButton * button, gpointer user_data)
{
   util_trig_activate(3);
}

void trgs_edit4_clicked(GtkButton * button, gpointer user_data)
{
   util_trig_activate(4);
}

void trgs_close_clicked(GtkButton * button, gpointer user_data)
{
   int i, j;
   gtk_widget_hide(gTrgsDialog);

   for(i=0; i< sizeof(gTrigSamplesText)/sizeof(gTrigSamplesText[0]); i++)
      {
         sscanf(gTrigSamplesText[i], "%d", &j);
         if(j == gTrigSamples)
            {
               gSettings.triggerSamples = i;
               break;
            }
      }

   for(i=0; i<PISCOPE_TRIGGERS; i++)
      {
         gSettings.triggers[i].enabled = gTrigInfo[i].enabled;
         gSettings.triggers[i].action = gTrigInfo[i].when;
         for (j=0; j<PISCOPE_GPIOS; j++)
            gSettings.triggers[i].gpiotypes[j] = gTrigInfo[i].type[j];
      }
   pigpioSaveSettings();
}

/* TRIG ------------------------------------------------------------------- */

void trig_apply_clicked(GtkButton * button, gpointer user_data)
{
   int i;

   if (!gTriggerNum) return;

   for (i=0; i<PISCOPE_GPIOS; i++)
      {
         gSettings.triggers[gTriggerNum-1].gpiotypes[i] = gtk_combo_box_get_active(GTK_COMBO_BOX(gTrigCombo[i]));
      }
   util_setTriggerGPIOTypes(gTriggerNum-1);

   trgs_lab_str(gTriggerNum-1);

   gtk_widget_hide(gTrigDialog);

   gTriggerNum = 0;
}

void trig_cancel_clicked(GtkButton * button, gpointer user_data)
{
   gtk_widget_hide(gTrigDialog);

   gTriggerNum = 0;
}

void trig_countsShow(void)
{
   static char symbol[]="#>~<";

   int i;
   char c[PISCOPE_TRIGGERS];
   char buf[128];

   for (i=0; i<PISCOPE_TRIGGERS; i++)
   {
      if (gTrigInfo[i].enabled) c[i] = symbol[gTrigInfo[i].when];
      else                      c[i] = ' ';
   }

   sprintf(buf, "1%c%Lu  2%c%Lu  3%c%Lu  4%c%Lu",
      c[0], (long long)gTrigInfo[0].count,
      c[1], (long long)gTrigInfo[1].count,
      c[2], (long long)gTrigInfo[2].count,
      c[3], (long long)gTrigInfo[3].count);

   util_labelText(gMainLtrigs, buf);
}

/* MAIN UTIL -------------------------------------------------------------- */

static void main_util_labelTick(int64_t *tick, GtkWidget *label)
{
   int64_t diff;
   int s, m;
   char buf[64];

   diff = (*tick) - gTickOrigin;

   s = diff/PISCOPE_MILLION;
   m = diff%PISCOPE_MILLION;

   sprintf(buf, "%d.%06d ", s, m);

   util_labelText(label, buf);
}

static void main_util_labelBlueTick(void)
{
   int64_t diff1, diff2;
   int s1, s2, m1, m2, sign;
   char buf[64];

   main_util_labelTick(&gBlueTick, gMainLblue);

   diff1 = gGoldTick - gTickOrigin;

   s1 = diff1/PISCOPE_MILLION;
   m1 = diff1%PISCOPE_MILLION;

   diff2 = gBlueTick - gGoldTick;

   sign = '+';

   if (diff2 < 0) {diff2 = -diff2; sign = '-';}

   s2 = diff2/PISCOPE_MILLION;
   m2 = diff2%PISCOPE_MILLION;

   if (s2) sprintf(buf, "%d.%06d (%c%d.%06d)", s1, m1, sign, s2, m2);
   else    sprintf(buf, "%d.%06d (%c%d us)", s1, m1, sign, m2);

   util_labelText(gMainLgold, buf);
}

static int main_util_checkTriggers(uint32_t new, uint32_t old)
{
   int i, matched;
   uint32_t changed;

   changed = new ^ old;
   matched = 0;

   for (i=0; i<PISCOPE_TRIGGERS; i++)
   {
      if (gTrigInfo[i].enabled)
      {
         if (((new&gTrigInfo[i].levelMask) == gTrigInfo[i].levelValue) &&
             ((gTrigInfo[i].changedMask&changed) == gTrigInfo[i].changedMask))
         {
            matched |= (1<<i);
         }
      }
   }

   return matched;

}

static void main_util_insertReport(gpioReport_t * report)
{
   static uint32_t lastLevel, lastTick;
   static uint16_t wrapCount;

   int triggered, i, samples;

   if (gBufWritePos < 0) /* first report */
   {
      gBufWritePos =  0;
      gBufReadPos  =  0;
      gBufSamples  =  1;

      /* make first report the time origin */

      gTickOrigin = report->tick;

      gGoldTick = gTickOrigin;

      gettimeofday(&gTimeOrigin, NULL);

      wrapCount = 0;

      lastTick  = report->tick;
      lastLevel = report->level;

      gSampleTick[0]  = lastTick;
      gSampleLevel[0] = lastLevel;
   }
   else
   {
      if ( (lastTick > 0xF0000000) && ((report->tick) < 0x10000000) )
      {
         wrapCount++;
      }

      lastTick  = report->tick;

   }

   if (report->level != lastLevel)
   {
      if (++gBufSamples > PISCOPE_SAMPLES)
      {
         /* buffer full */

         gBufSamples = PISCOPE_SAMPLES;

         if (gMode == piscope_live)
         {
            if (++gBufReadPos >= PISCOPE_SAMPLES) gBufReadPos = 0;
         }
         else
         {
            /* simply ignore new samples */
            return;
         }
      }

      if ((triggered = main_util_checkTriggers(report->level, lastLevel)))
      {
         for (i=0; i<PISCOPE_TRIGGERS; i++)
         {
            if (triggered & (1<<i))
            {
               gTrigInfo[i].count++;

               if (gMode == piscope_live)
               {
                  if (!gTrigInfo[i].fired)
                  {
                     samples = 0;

                     switch(gTrigInfo[i].when)
                     {
                        case piscope_count:
                           break;

                        case piscope_sample_from:
                           gTriggerFired = 1;
                           gTrigInfo[i].fired = 1;
                           samples = gTrigSamples;
                           break;

                        case piscope_sample_around:
                           gTriggerFired = 1;
                           gTrigInfo[i].fired = 1;
                           samples = gTrigSamples/2;
                           break;

                        case piscope_sample_to:
                           gTriggerFired = 1;
                           gTrigInfo[i].fired = 1;
                           break;
                     }

                     if (samples > gTriggerCount) gTriggerCount = samples;
                  }
               }
            }
         }
      }

      lastLevel  = report->level;

      if (++gBufWritePos >= PISCOPE_SAMPLES) gBufWritePos = 0;

      gSampleTick[gBufWritePos]  = ((uint64_t)wrapCount<<32)|lastTick;
      gSampleLevel[gBufWritePos] = lastLevel;

      if ((gMode == piscope_live) && gTriggerFired)
      {
         if (--gTriggerCount < 0)
         {
            gMode = piscope_pause;

            gtk_toggle_tool_button_set_active
               (GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);
         }
      }
   }
}

static gboolean main_util_input(gpointer user_data)
{
   static int reportsPerCycle = 2000;
   static int got = 0;

   struct timeval tv = { 0L, 0L };
   struct timeval t1, t2, tDiff;

   int reports, bytes, micros, r;

   fd_set fds;

   if (gInputState == piscope_initialise)
   {
      got = 0;
      gInputState = piscope_running;
   }
   else if (gInputState == piscope_quit)    {gtk_main_quit(); return FALSE;}
   else if (gInputState == piscope_dormant) return TRUE;

   gettimeofday(&t1, NULL);

   reports = 0;

   while (reports <= reportsPerCycle)
   {

      FD_ZERO(&fds);

      if (gPigNotify >= 0) FD_SET(gPigNotify, &fds);

      if (select(gPigNotify+1, &fds, NULL, NULL, &tv) != 1) break;

      bytes = read(gPigNotify, (char*)&gReport+got, sizeof(gReport)-got);

      if (bytes > 0)
      {
         got += bytes;
      }
      else
      {
         break;
      }

      r = 0;

      while (got >= sizeof(gpioReport_t))
      {
         main_util_insertReport(&gReport[r]);

         r++;

         reports++;

         got -= sizeof(gpioReport_t);
      }

      /* copy any partial report to start of array */

      if (got && r) gReport[0] = gReport[r];
   }

   if (reports >= 500)
   {
      gettimeofday(&t2, NULL);

      timersub(&t2, &t1, &tDiff);

      micros = (tDiff.tv_sec * PISCOPE_MILLION) + tDiff.tv_usec;

      r = (gTimeSlotMicros*reports)/micros;

      r = (80 * r) / 100;  /* give some spare time in  slot */

      if (r > reportsPerCycle)
      {
         reportsPerCycle = r;
      }
   }

   return TRUE;
}

static void main_util_searchEdge(int dir)
{
   uint32_t mask, oldLevel, newLevel;
   int found, first, s;

   if (gHilitGpios) mask = gHilitGpios; else mask = -1;

   if (gBlueTick)
   {
      if (dir)
      {
         s = gViewStartSample;

         found = 0;

         oldLevel = gSampleLevel[s] & mask;

         while (!found && (s != gBufWritePos))
         {
            if (gSampleTick[s] <= gBlueTick)
            {
               oldLevel = gSampleLevel[s] & mask;
            }
            else
            {
               newLevel = gSampleLevel[s] & mask;

               if (newLevel != oldLevel)
               {
                  found = 1;

                  gBlueTick = gSampleTick[s];

                  if (gBlueTick > gViewEndTick)
                     gViewCentreTick = gBlueTick + (0.4 * gViewTicks);
               }
            }
            if (++s >= PISCOPE_SAMPLES) s -= PISCOPE_SAMPLES;
         }
      }
      else
      {
         s = gViewEndSample;

         found = 0;
         first = 1;

         oldLevel = gSampleLevel[s] & mask;

         while (!found && (s != gBufReadPos))
         {
            if (gSampleTick[s] < gBlueTick)
            {
               if (first)
               {
                  first = 0;
                  oldLevel = gSampleLevel[s] & mask;
               }
               else
               {
                  newLevel = gSampleLevel[s] & mask;

                  if (newLevel != oldLevel)
                  {
                     found = 1;

                     if (s < PISCOPE_SAMPLES)
                        gBlueTick = gSampleTick[s+1];
                     else gBlueTick = gSampleTick[0];

                     if (gBlueTick < gViewStartTick)
                        gViewCentreTick = gBlueTick - (0.4 * gViewTicks);
                  }
               }
            }
            if (--s < 0) s += PISCOPE_SAMPLES;
         }
      }

      main_util_labelBlueTick();

   }
}

static void main_util_searchTrigger(int dir)
{
   uint32_t old, new;
   int found, s;

   if (gBlueTick)
   {
      if (dir)
      {
         s = gViewStartSample;
         old = gSampleLevel[s];

         found = 0;

         while (!found && (s != gBufWritePos))
         {
            new = gSampleLevel[s];

            if (gSampleTick[s] > gBlueTick)
            {
               found = main_util_checkTriggers(new, old);

               if (found)
               {
                  gBlueTick = gSampleTick[s];

                  if (gBlueTick > gViewEndTick)
                     gViewCentreTick = gBlueTick + (0.4 * gViewTicks);
               }
            }
            old = new;
            if (++s >= PISCOPE_SAMPLES) s -= PISCOPE_SAMPLES;
         }
      }
      else
      {
         s = gViewEndSample;
         new = gSampleLevel[s];

         found = 0;

         while (!found && (s != gBufReadPos))
         {
            old = gSampleLevel[s];

            if (gSampleTick[s] < gBlueTick)
            {
               found = main_util_checkTriggers(new, old);

               if (found)
               {
                  if ((gBlueTick != gSampleTick[s+1]) &&
                      (gSampleTick[s+1] < gBlueTick))
                  {
                     gBlueTick = gSampleTick[s+1];

                     if (gBlueTick < gViewStartTick)
                        gViewCentreTick = gBlueTick - (0.4 * gViewTicks);
                  }
                  else found = 0;
               }
            }

            new = old;

            if (--s < 0) s += PISCOPE_SAMPLES;
         }
      }

      main_util_labelBlueTick();

   }
}

static int main_util_bsearch(int s1, int s2, int64_t *tick)
{
   int mid, mida;

   if (s2 < s1) s2 += PISCOPE_SAMPLES;

   while (s1 < s2)
   {
      mid = s1 + (s2 - s1) / 2;

      mida = mid;

      if (mid >= PISCOPE_SAMPLES) mida -= PISCOPE_SAMPLES;

      if (gSampleTick[mida] < (*tick)) s1 = mid + 1;
      else                             s2 = mid;
   }

   if (s1 >= PISCOPE_SAMPLES) s1 -= PISCOPE_SAMPLES;

   return s1;
}


static void main_util_1Tick(void)
{
   int64_t x, diffTick;

   if ((g1Tick > gViewStartTick) && (g1Tick < gViewEndTick))
   {
      cairo_set_line_width(gCoscCairo, 1.0);

      cairo_set_source_rgb(gCoscCairo, 0.3, 1.0, 0.3);

      diffTick = g1Tick - gViewStartTick;

      x = (int64_t)10 * diffTick / (int64_t)gDeciMicroPerPix;

      cairo_move_to(gCoscCairo, x, 0);

      cairo_line_to(gCoscCairo, x, gCoscHeight);

      cairo_stroke(gCoscCairo);
   }
}


static void main_util_2Tick(void)
{
   int64_t x, diffTick;

   if ((g2Tick > gViewStartTick) && (g2Tick < gViewEndTick))
   {
      cairo_set_line_width(gCoscCairo, 1.0);

      cairo_set_source_rgb(gCoscCairo, 1.0, 0.3, 0.3);

      diffTick = g2Tick - gViewStartTick;

      x = (int64_t)10 * diffTick / (int64_t)gDeciMicroPerPix;

      cairo_move_to(gCoscCairo, x, 0);

      cairo_line_to(gCoscCairo, x, gCoscHeight);

      cairo_stroke(gCoscCairo);
   }
}


static void main_util_GoldTick(void)
{
   int64_t x, diffTick;

   if ((gGoldTick > gViewStartTick) && (gGoldTick < gViewEndTick))
   {
      cairo_set_line_width(gCoscCairo, 1.0);

      cairo_set_source_rgb(gCoscCairo, 1.0, 1.0, 0.3);

      diffTick = gGoldTick - gViewStartTick;

      x = (int64_t)10 * diffTick / (int64_t)gDeciMicroPerPix;

      cairo_move_to(gCoscCairo, x, 0);

      cairo_line_to(gCoscCairo, x, gCoscHeight);

      cairo_stroke(gCoscCairo);
   }
}


static void main_util_BlueTick(void)
{
   int64_t x, diffTick;

   if ((gBlueTick > gViewStartTick) && (gBlueTick < gViewEndTick))
   {
      cairo_set_line_width(gCoscCairo, 1.0);

      cairo_set_source_rgb(gCoscCairo, 0.3, 1.0, 1.0);

      diffTick = gBlueTick - gViewStartTick;

      x = (int64_t)(10 * diffTick) / (int64_t)gDeciMicroPerPix;

      cairo_move_to(gCoscCairo, x, 0);

      cairo_line_to(gCoscCairo, x, gCoscHeight);

      cairo_stroke(gCoscCairo);
   }
}


static void main_util_display(void)
{
   static int rollingAverage = 0;
   int g, s, millis;
   int lev1, lev2;
   struct timeval t1, t2, tDiff;

   int64_t x1, x2, y1, y2, y3;
   int64_t diffTick;

   if (rollingAverage > 40)
   {
      rollingAverage = ((9 * rollingAverage) / 10);
      return;
   }

   gettimeofday(&t1, NULL);

   cairo_set_source_rgb(gCoscCairo, 0.0, 0.0, 0.0);

   cairo_paint(gCoscCairo);

   for (g=0; g<PISCOPE_GPIOS; g++)
   {
      if (gGpioInfo[g].display)
      {
         cairo_set_line_width(gCoscCairo, 0.5);

         cairo_set_source_rgb(gCoscCairo, 1.0, 1.0, 1.0);

         cairo_move_to(gCoscCairo, 0, gGpioInfo[g].y_tick);

         cairo_line_to(gCoscCairo, gCoscWidth, gGpioInfo[g].y_tick);

         cairo_stroke(gCoscCairo);

         cairo_set_line_width(gCoscCairo, 2.0);

         cairo_set_source_rgb(gCoscCairo, 0.2, 0.6, 0.1);

         s = gViewStartSample;

         lev1 = gSampleLevel[s] & (1<<g);

         x1 = 0;
         y1 = 0;

         y2 = 0;

         if (lev1 == 0) y1 = gGpioInfo[g].y_low;
         else           y1 = gGpioInfo[g].y_high;

         cairo_move_to(gCoscCairo, x1, y1);

         y3 = -1;

         while (s != gViewEndSample)
         {
            if (++s >= PISCOPE_SAMPLES) s = 0;

            lev2 = gSampleLevel[s] & (1<<g);

            if (lev1 != lev2)
            {
               /* line height change */

               diffTick = gSampleTick[s] - gViewStartTick;

               x2 = (int64_t)(10 * diffTick) / (int64_t)gDeciMicroPerPix;

               if (x2 != x1)
               {
                  if (y3 != -1)
                  {
                     cairo_move_to(gCoscCairo, x1, y3);
                     y1 = y3;
                     y3 = -1;
                  }

                  cairo_line_to(gCoscCairo, x2, y1);

                  if (lev2 == 0) y2 = gGpioInfo[g].y_low;
                  else           y2 = gGpioInfo[g].y_high;

                  cairo_line_to(gCoscCairo, x2, y2);
               }
               else
               {
                  if (lev2 == 0) y3 = gGpioInfo[g].y_low;
                  else           y3 = gGpioInfo[g].y_high;
               }

               lev1 = lev2;
               x1   = x2;
               y1   = y2;
            }
         }

         if (y3 != -1)
         {
            cairo_move_to(gCoscCairo, x1, y3);
            y1 = y3;
            y3 = -1;
         }

         /* finish line at screen edge */

         cairo_line_to(gCoscCairo, gCoscWidth, y1);

         cairo_stroke(gCoscCairo);
      }
   }

   main_util_GoldTick();

   main_util_BlueTick();

   main_util_1Tick();

   main_util_2Tick();

   /* redraw screen */

   gtk_widget_queue_draw(gMainCosc);

   gettimeofday(&t2, NULL);

   timersub(&t2, &t1, &tDiff);

   millis = ((tDiff.tv_sec * PISCOPE_MILLION) + tDiff.tv_usec) / 1000;

   rollingAverage = ((9 * rollingAverage) + millis) / 10;
}

static void main_util_samp_show(void)
{
   cairo_t *cr;
   int bufUsedPix;
   int width, start, widthPix,startPix;

   width = gViewEndSample - gViewStartSample;
   start = gViewStartSample - gBufReadPos;

   if (width < 0) width += PISCOPE_SAMPLES;
   if (start < 0) start += PISCOPE_SAMPLES;

   startPix   = (gCsampWidth * start)       / PISCOPE_SAMPLES;
   widthPix   = (gCsampWidth * width)       / PISCOPE_SAMPLES;
   bufUsedPix = (gCsampWidth * gBufSamples) / PISCOPE_SAMPLES;

   if (widthPix < 2) widthPix = 2;

   cr = cairo_create(gCsampSurface);

   cairo_set_source_rgb(cr, 0.9, 0.8, 0.7);

   cairo_paint(cr);

   cairo_set_source_rgb(cr, 0.6, 0.5, 0.4);

   cairo_rectangle(cr, 0, 0, bufUsedPix, gCsampHeight);

   cairo_fill(cr);

   cairo_set_source_rgb(cr, 0.3, 0.2, 0.1);

   cairo_rectangle(cr, startPix, 0, widthPix, gCsampHeight);

   cairo_fill(cr);

   cairo_destroy(cr);

   gtk_widget_queue_draw(gMainCbuf);
}


static gboolean main_util_output(gpointer data)
{
   int decimals, blue;
   char buf[128];

   if (gOutputState == piscope_initialise)
      gOutputState = piscope_running;

   else if (gOutputState == piscope_quit)
   {
      gtk_main_quit();
      return FALSE;
   }

   else if (gOutputState == piscope_dormant)
      return TRUE;

   /* don't start display until data has arrived */

   if (gBufWritePos < 0) return TRUE;

   gFirstReportTick = gSampleTick[gBufReadPos];
   gLastReportTick  = gSampleTick[gBufWritePos];

   if (gMode == piscope_live)
   {
      decimals = 1;
      blue = 1;

      gViewEndTick =
         (gLastReportTick / gRefreshTicks) * gRefreshTicks;

      gViewCentreTick = gViewEndTick - (gViewTicks/2);

      gViewStartTick  = gViewEndTick - gViewTicks;
   }
   else if (gMode == piscope_play)
   {
      decimals = (gPlaySpeed / 3) - 1;

      if      (decimals < 0) decimals = 0;
      else if (decimals > 6) decimals = 6;

      blue = 1;

      gViewCentreTick +=
         ((gRefreshTicks * (1<<PISCOPE_DEF_SPEED_IDX))/(1<<gPlaySpeed));

      gViewEndTick   = gViewCentreTick + (gViewTicks/2);

      gViewStartTick = gViewEndTick    - gViewTicks;
   }
   else
   {
      /* MODE_PAUSE */

      decimals = 6;
      blue = 0;

      gViewEndTick   = gViewCentreTick + (gViewTicks/2);

      gViewStartTick = gViewEndTick    - gViewTicks;
   }

   if (gViewStartTick > gFirstReportTick)
   {
      gViewStartSample =
         main_util_bsearch(gBufReadPos, gBufWritePos, &gViewStartTick);
   }
   else
   {
      gViewStartSample = gBufReadPos;
      gViewStartTick   = gFirstReportTick;
      gViewEndTick     = gViewStartTick + gViewTicks;
      gViewCentreTick  = gViewEndTick   - (gViewTicks/2);
   }

   if (gViewStartSample != gBufReadPos)
   {
     if (--gViewStartSample < 0) gViewStartSample = PISCOPE_SAMPLES - 1;
   }

   if (gViewEndTick < gLastReportTick)
   {
      gViewEndSample =
         main_util_bsearch(gBufReadPos, gBufWritePos, &gViewEndTick);
   }
   else
   {
      gViewEndSample  = gBufWritePos;
      gViewEndTick    = gLastReportTick;
      gViewStartTick  = gViewEndTick - gViewTicks;
      gViewCentreTick = gViewEndTick - (gViewTicks/2);
   }

   main_util_display();
   main_util_samp_show();

   strcpy(buf, util_timeStamp(&gViewEndTick, decimals, blue));

   util_labelText(gMainLtime, buf);

   trig_countsShow();

   return TRUE;
}

/* MAIN HLEG -------------------------------------------------------------- */

gboolean main_hleg_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
   if (gChlegSurface)
   {
      cairo_set_source_surface(cr, gChlegSurface, 0, 0);

      cairo_paint(cr);
   }

   return FALSE;
}

void main_hleg_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
)
{
   util_hlegConfigure(widget);
}

/* MAIN MODE -------------------------------------------------------------- */

void main_mode_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
)
{
   if (gCmodeSurface) cairo_surface_destroy(gCmodeSurface);

   gCmodeWidth  = gtk_widget_get_allocated_width(widget);
   gCmodeHeight = gtk_widget_get_allocated_height(widget);

   gCmodeSurface = gdk_window_create_similar_surface
   (
      gtk_widget_get_window(widget),
      CAIRO_CONTENT_COLOR,
      gCmodeWidth,
      gCmodeHeight
   );

   util_mode_display();
}

gboolean main_mode_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
   if (gCmodeSurface)
   {
      cairo_set_source_surface(cr, gCmodeSurface, 0, 0);

      cairo_paint(cr);
   }

   return FALSE;
}

/* MAIN OSC --------------------------------------------------------------- */

gboolean main_osc_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
)
{
   gCoscWidth = gtk_widget_get_allocated_width(widget);

   gViewTicks = (((int64_t)gCoscWidth * gDeciMicroPerPix)/10);

   gCoscHeight = gtk_widget_get_allocated_height(widget);

   util_calcGpioY();

   if (gCoscSurface) cairo_surface_destroy(gCoscSurface);

   gCoscSurface = gdk_window_create_similar_surface
   (
      gtk_widget_get_window(widget),
      CAIRO_CONTENT_COLOR,
      gCoscWidth,
      gCoscHeight
   );

   if (gCoscCairo) cairo_destroy(gCoscCairo);

   gCoscCairo = cairo_create(gCoscSurface);

   cairo_set_source_rgb(gCoscCairo, 0.0, 0.0, 0.0);

   cairo_paint(gCoscCairo);

   return TRUE;
}

gboolean main_osc_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
   if (gCoscSurface)
   {
      cairo_set_source_surface(cr, gCoscSurface, 0, 0);

      cairo_paint(cr);
   }

   return FALSE;
}

gboolean main_osc_motion_notify_event(
   GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
   int64_t centre, diff, ticks;

   if (event->type == GDK_MOTION_NOTIFY)
   {
      if (gMode == piscope_pause)
      {
         centre = gCoscWidth / 2;

         diff = event->x - centre;

         ticks = (diff * gViewTicks) / gCoscWidth;

         gBlueTick = gViewCentreTick + ticks;

         main_util_labelBlueTick();
      }

   }

   return TRUE;
}

gboolean main_osc_button_press_event(
   GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
   int64_t centre, diff, ticks;

   if (event->type == GDK_BUTTON_PRESS)
   {
      if (event->button == 1)
      {
         centre = gCoscWidth / 2;

         diff = event->x - centre;

         ticks = (diff * gViewTicks) / gCoscWidth;

         gGoldTick = gViewCentreTick + ticks;

         if (gGoldTick < gTickOrigin) gGoldTick = gTickOrigin;

         main_util_labelTick(&gGoldTick, gMainLgold);
      }
   }
   else if (event->type == GDK_2BUTTON_PRESS)
   {
      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

      centre = gCoscWidth / 2;

      diff = event->x - centre;

      ticks = (diff * gViewTicks) / gCoscWidth;

      gViewCentreTick += ticks;
   }

  return TRUE;
}



/* MAIN SAMP -------------------------------------------------------------- */

gboolean main_samp_button_press_event(
   GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
   int sample;

   sample = (event->x * PISCOPE_SAMPLES) / gCsampWidth;

   if (sample >= gBufSamples) sample = gBufSamples - 1;

   if (sample < 0) sample = 0;

   sample += gBufReadPos;

   if (sample >= PISCOPE_SAMPLES) sample -= PISCOPE_SAMPLES;

   if (event->type == GDK_BUTTON_PRESS)
   {
      if (gMode == piscope_pause)
      {
         gViewCentreTick = gSampleTick[sample];

         gGoldTick = gViewCentreTick;

         main_util_labelTick(&gGoldTick, gMainLgold);
      }
   }
   else if (event->type == GDK_2BUTTON_PRESS)
   {
      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

      gViewCentreTick = gSampleTick[sample];

      gGoldTick = gViewCentreTick;

      main_util_labelTick(&gGoldTick, gMainLgold);
   }

  return TRUE;
}

void main_samp_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
)
{
   cairo_t *cr;

   if (gCsampSurface) cairo_surface_destroy(gCsampSurface);

   gCsampWidth  = gtk_widget_get_allocated_width(widget);
   gCsampHeight = gtk_widget_get_allocated_height(widget);

   gCsampSurface = gdk_window_create_similar_surface
   (
      gtk_widget_get_window(widget),
      CAIRO_CONTENT_COLOR,
      gCsampWidth,
      gCsampHeight
   );

   /* Initialize the surface to grey */

   cr = cairo_create(gCsampSurface);

   cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

   cairo_paint(cr);

   cairo_destroy(cr);
}

gboolean main_samp_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
   if (gCsampSurface)
   {
      cairo_set_source_surface(cr, gCsampSurface, 0, 0);

      cairo_paint(cr);
   }

   return FALSE;
}

/* MAIN MENU -------------------------------------------------------------- */

void main_destroy(void)
{
   gInputState  = piscope_quit;
   gOutputState = piscope_quit;

   if (gPigSocket >= 0)
   {
      if (gPigHandle >= 0)
      {
         pigpioCommand(gPigSocket, PI_CMD_NC, gPigHandle, 0);
      }

      close(gPigSocket);
   }

   if (gPigNotify >= 0) close(gPigNotify);
}

void main_menu_file_restore_activate
   (GtkMenuItem *menuitem, gpointer user_data)
{
   GtkWidget *dialog;
   char *filename;

   GtkFileFilter *txt;

   txt = gtk_file_filter_new();
   gtk_file_filter_set_name(txt, "TEXT");
   gtk_file_filter_add_mime_type(txt, "text/plain");

   dialog = gtk_file_chooser_dialog_new
   (
      "Restore Saved Samples",
       GTK_WINDOW(gMain),
       GTK_FILE_CHOOSER_ACTION_OPEN,
       MY_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
       MY_STOCK_OPEN,   GTK_RESPONSE_ACCEPT,
       NULL
   );

   gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), txt);

   if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
   {

      filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

      file_load(filename);

      g_free(filename);
   }

   gtk_widget_destroy(dialog);
}

void main_menu_file_save(GtkMenuItem *menuitem, int selection)
{
   GtkWidget *dialog;
   char *filename;
   GtkFileFilter *vcd, *txt;
   int textfile;
   char *title1 = "Save All Samples";
   char *title2 = "Save Selected Samples";
   char *title;

   if (selection) title = title2; else title = title1;

   txt = gtk_file_filter_new();
   gtk_file_filter_set_name(txt, "TEXT");
   gtk_file_filter_add_mime_type(txt, "text/plain");

   vcd = gtk_file_filter_new();
   gtk_file_filter_set_name(vcd, "VCD");
   gtk_file_filter_add_mime_type(txt, "text/plain");

   dialog = gtk_file_chooser_dialog_new
   (
      title,
      GTK_WINDOW(gMain),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      MY_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
      MY_STOCK_SAVE,   GTK_RESPONSE_ACCEPT,
      NULL
   );

   gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), txt);
   gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), vcd);

   gtk_file_chooser_set_do_overwrite_confirmation(
      GTK_FILE_CHOOSER(dialog), TRUE);

   if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
   {
      filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

      if (gtk_file_chooser_get_filter(GTK_FILE_CHOOSER(dialog)) == vcd)
         textfile = 0;
      else
         textfile = 1;

      if (strlen(filename) > 3)
      {
         if (strcasecmp(".vcd", filename + strlen(filename) - 4) == 0)
            textfile = 0;
      }

      if (strlen(filename) > 7)
      {
         if (strcasecmp(".piscope", filename + strlen(filename) - 8) == 0)
            textfile = 1;
      }

      if (textfile)
         file_save(piscope_text, filename, selection);
      else
         file_save(piscope_vcd, filename, selection);

      g_free(filename);
   }

   gtk_widget_destroy(dialog);
}

void main_menu_file_save_all_activate
   (GtkMenuItem *menuitem, gpointer user_data)
{
   main_menu_file_save(menuitem, 0);
}

void main_menu_file_save_selection_activate
   (GtkMenuItem *menuitem, gpointer user_data)
{
   main_menu_file_save(menuitem, 1);
}

void main_menu_file_quit_activate(GtkMenuItem *menuitem, gpointer user_data)
{
   main_destroy();
}

void main_menu_misc_gpios_activate(GtkMenuItem *menuitem, gpointer user_data)
{
   int i;

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gGpioTempDisplay[i] =
         gtk_toggle_button_get_active(gGpioInfo[i].button);
   }

   gtk_widget_show(gGpioDialog);
}

void main_menu_misc_triggers_activate(GtkMenuItem *menuitem, gpointer user_data)
{
   int i;

   for (i=0; i<PISCOPE_TRIGGERS; i++) trgs_lab_str(i);

   gtk_widget_show(gTrgsDialog);
}

void main_menu_help_about_activate
(
   GtkMenuItem *menuitem,
   gpointer user_data
)
{
   gtk_show_about_dialog
   (
      GTK_WINDOW(gMain),
      "program-name", "piscope",
      "title", "About piscope",
      "version", PISCOPE_VERSION,
      "website", "http://abyz.me.uk/rpi/pigpio/piscope.html",
      "website-label", "piscope",
      "comments", "A digital waveform viewer for the Raspberry",
      NULL
   );
}

/* MAIN TB ---------------------------------------------------------------- */

void main_tb_connect_toggled(GtkToggleToolButton * button, gpointer user_data)
{
   char msg[64];
   if (gtk_toggle_tool_button_get_active(button))
   {
      pigpioConnect(); // can fail

      if (gPigConnected)
      {
         gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(button), "gtk-disconnect");
         gtk_tool_button_set_label(GTK_TOOL_BUTTON(button), "Disconnect");
         snprintf(msg, sizeof(msg), "Disconnect from pigpio at %s",
            gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr)));
      
         gtk_widget_set_tooltip_text(GTK_WIDGET(button), msg);
      }
   }
   else
   {
      pigpioDisconnect(); // can't fail

      gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(button), "gtk-connect");
      gtk_tool_button_set_label(GTK_TOOL_BUTTON(button), "Connect");
      snprintf(msg, sizeof(msg), "Connect to pigpio at %s",
         gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr)));
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), msg);
   }
}

void main_tb_pause_toggled(GtkToggleToolButton * button, gpointer user_data)
{
   if (gtk_toggle_tool_button_get_active(button))
   {
      util_setViewMode(piscope_pause);

      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBplay), FALSE);
      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBlive), FALSE);
   }
   else
   {
      if (gMode == piscope_pause)
         gtk_toggle_tool_button_set_active(button, TRUE);
   }
}

void main_tb_play_toggled(GtkToggleToolButton * button, gpointer user_data)
{
   if (gtk_toggle_tool_button_get_active(button))
   {
      util_setViewMode(piscope_play);

      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), FALSE);
      gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBlive), FALSE);
   }
   else
   {
      if (gMode == piscope_play)
         gtk_toggle_tool_button_set_active(button, TRUE);
   }
}

void main_tb_live_toggled(GtkToggleToolButton * button, gpointer user_data)
{
   if (gtk_toggle_tool_button_get_active(button))
   {
      trgs_reset();

      if (gPigConnected)
      {
         util_setViewMode(piscope_live);

         gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), FALSE);
         gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBplay), FALSE);
      }
      else
      {
         util_popupMessage(GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
         "You must connect to pigpio before going LIVE.");

         gtk_toggle_tool_button_set_active(button, FALSE);
      }
   }
   else
   {
      if (gMode == piscope_live)
         gtk_toggle_tool_button_set_active(button, TRUE);
   }
}

void main_tb_settings_clicked(GtkButton * button, gpointer user_data)
{
   gtk_widget_show(gCmdsDialog);
}

void main_tb_speed_def_clicked(GtkButton * button, gpointer user_data)
{
   gPlaySpeed = PISCOPE_DEF_SPEED_IDX;
}

void main_tb_speed_up_clicked(GtkButton * button, gpointer user_data)
{
   if (gPlaySpeed > PISCOPE_MIN_SPEED_IDX) --gPlaySpeed;
}

void main_tb_speed_down_clicked(GtkButton * button, gpointer user_data)
{
   if (gPlaySpeed < PISCOPE_MAX_SPEED_IDX) ++gPlaySpeed;
}

void main_tb_zoom_def_clicked(GtkButton * button, gpointer user_data)
{
   if (gZoomLevel != PISCOPE_DEFAULT_ZOOM_LEVEL)
      util_zoom_def_clicked();
}

void main_tb_zoom_in_clicked(GtkButton * button, gpointer user_data)
{
   if (gZoomLevel > 0)
   {
      --gZoomLevel;

      gDeciMicroPerPix = gZoomDeciMicroPerPix[gZoomLevel];

      gViewTicks = (((int64_t)gCoscWidth * gDeciMicroPerPix)/10);

      util_hlegConfigure(gMainChleg);

      gtk_widget_queue_draw(gMainChleg);
   }
}

void main_tb_zoom_out_clicked(GtkButton * button, gpointer user_data)
{
   if (gZoomLevel < ((sizeof(gZoomDeciMicroPerPix)/sizeof(int))-1))
   {
      ++gZoomLevel;

      gDeciMicroPerPix = gZoomDeciMicroPerPix[gZoomLevel];

      gViewTicks = (((int64_t)gCoscWidth * gDeciMicroPerPix)/10);

      util_hlegConfigure(gMainChleg);

      gtk_widget_queue_draw(gMainChleg);
   }
}

void main_tb_first_clicked(GtkButton * button, gpointer user_data)
{
   util_setViewMode(piscope_pause);

   gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

   gViewCentreTick = gSampleTick[gBufReadPos] + (gViewTicks/2);
}

void main_tb_last_clicked(GtkButton * button, gpointer user_data)
{
   util_setViewMode(piscope_pause);

   gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

   gViewCentreTick = gSampleTick[gBufWritePos] - (gViewTicks/2);
}

void main_tb_back_clicked(GtkButton * button, gpointer user_data)
{
   util_setViewMode(piscope_pause);

   gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

   gViewCentreTick -= (9 * gViewTicks) / 10;
}

void main_tb_forward_clicked(GtkButton * button, gpointer user_data)
{
   util_setViewMode(piscope_pause);

   gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(gMainTBpause), TRUE);

   gViewCentreTick += (9 * gViewTicks) / 10;
}

/* MAIN VLEG -------------------------------------------------------------- */

void main_vleg_configure_event
(
   GtkWidget         *widget,
   GdkEventConfigure *event,
   gpointer           data
)
{
   util_vlegConfigure(gMainCvleg);
}

gboolean main_vleg_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
   if (gCvlegSurface)
   {
      cairo_set_source_surface(cr, gCvlegSurface, 0, 0);

      cairo_paint(cr);
   }

   return FALSE;
}

gboolean main_vleg_button_press_event(
   GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
   int i, y;

   if (event->type == GDK_BUTTON_PRESS)
   {
      if (event->button == 1)
      {
         y = event->y;

         for (i=0; i<PISCOPE_GPIOS; i++)
         {
            if (gGpioInfo[i].display)
            {
               if ((y >= gGpioInfo[i].y_high) && (y <= gGpioInfo[i].y_tick))
               {
                  gGpioInfo[i].hilit = !gGpioInfo[i].hilit;

                  util_vlegConfigure(gMainCvleg);
                  gtk_widget_queue_draw(gMainCvleg);
               }
            }
         }
      }
   }

  return TRUE;
}

/* MAIN KEY --------------------------------------------------------------- */

gboolean main_key_press_event(
   GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
   switch (event->keyval)
   {
      case GDK_KEY_bracketleft:
         if (gMode == piscope_pause) main_util_searchTrigger(0);
         break;

      case GDK_KEY_bracketright:
         if (gMode == piscope_pause) main_util_searchTrigger(1);
         break;

      case GDK_KEY_Left:
         if (gMode == piscope_pause) main_util_searchEdge(0);
         break;

      case GDK_KEY_Right:
         if (gMode == piscope_pause) main_util_searchEdge(1);
         break;

      case GDK_KEY_Down:
         main_tb_zoom_in_clicked(NULL, NULL);
         break;

      case GDK_KEY_Up:
         main_tb_zoom_out_clicked(NULL, NULL);
         break;

      case GDK_KEY_Page_Down:
         if (gMode == piscope_play)
         {
            main_tb_speed_down_clicked(NULL, NULL);
            util_setViewMode(piscope_play);
         }
         break;

      case GDK_KEY_Page_Up:
         if (gMode == piscope_play)
         {
            main_tb_speed_up_clicked(NULL, NULL);
            util_setViewMode(piscope_play);
         }
         break;

      case GDK_KEY_Home:
         if (gMode == piscope_play)
         {
            main_tb_speed_def_clicked(NULL, NULL);
            util_setViewMode(piscope_play);
         }
         break;

      case GDK_KEY_d:
         ++gDebugLevel;
         if (gDebugLevel > 5) gDebugLevel = 5;
         break;

      case GDK_KEY_D:
         --gDebugLevel;
         if (gDebugLevel < 0) gDebugLevel = 0;
         break;

      case GDK_KEY_1:

         if (!g1Tick) g1Tick = gBlueTick;
         if (!g2Tick) g2Tick = gBlueTick;

         if (gBlueTick <= g2Tick)
         {
            g1Tick = gBlueTick;
         }
         else
         {
            g1Tick = g2Tick;
            g2Tick = gBlueTick;
         }
         break;

      case GDK_KEY_2:

         if (!g1Tick) g1Tick = gBlueTick;
         if (!g2Tick) g2Tick = gBlueTick;

         if (gBlueTick >= g1Tick)
         {
            g2Tick = gBlueTick;
         }
         else
         {
            g2Tick = g1Tick;
            g1Tick = gBlueTick;
         }
         break;

      case GDK_KEY_g:
      case GDK_KEY_G:
         gGoldTick = gBlueTick;
         break;
   }

  return FALSE;
}

void main_util_setWindowTitle()
{
   char *title = "piscope (http://abyz.me.uk/rpi/pigpio/piscope.html)";
   char buf[128];

   if (gPigConnected)
   {
      snprintf(buf, sizeof(buf), "%s   [%s:%s]",
         title,
         gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioAddr)),
         gtk_entry_get_text(GTK_ENTRY(gCmdsPigpioPort)));
   }
   else
   {
      snprintf(buf, sizeof(buf), "%s", title);
   }

   gtk_window_set_title(GTK_WINDOW(gMain), buf);
}
/* MAIN ------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
   GtkBuilder *builder;

   char buf[32];

   int  i, j, ui_ok;

   gtk_init(&argc, &argv);

   pigpioLoadSettings();

  /* Construct a GtkBuilder instance and load our UI description */

   builder = gtk_builder_new();

   ui_ok = 0;

   if (!access("piscope.glade", R_OK))
   {
      ui_ok = gtk_builder_add_from_file
      (
         builder,
         "piscope.glade",
         NULL
      );
   }
   else if (!access("/usr/share/piscope/piscope.glade", R_OK))
   {
      ui_ok = gtk_builder_add_from_file
      (
         builder,
         "/usr/share/piscope/piscope.glade",
         NULL
      );
   }

   if (!ui_ok)
   {
      fprintf(stderr, "\nFATAL ERROR: corrupt or missing UI (piscope.glade)\n");
      exit(1);
   }
   gtk_builder_connect_signals(builder, NULL);

   /* these widgets are accessed */

   PISCOPE_BUILDOBJ(gCmdsDialog);
   PISCOPE_BUILDOBJ(gCmdsPigpioAddr);
   PISCOPE_BUILDOBJ(gCmdsPigpioPort);
   PISCOPE_BUILDOBJ(gCmdsPlayspeed);

   PISCOPE_BUILDOBJ(gGpioDialog);

   PISCOPE_BUILDOBJ(gMain);

   PISCOPE_BUILDOBJ(gMainCbuf);
   PISCOPE_BUILDOBJ(gMainChleg);
   PISCOPE_BUILDOBJ(gMainCmode);
   PISCOPE_BUILDOBJ(gMainCosc);
   PISCOPE_BUILDOBJ(gMainCvleg);

   PISCOPE_BUILDOBJ(gMainLblue);
   PISCOPE_BUILDOBJ(gMainLgold);
   PISCOPE_BUILDOBJ(gMainLmode);
   PISCOPE_BUILDOBJ(gMainLtime);
   PISCOPE_BUILDOBJ(gMainLtrigs);

   PISCOPE_BUILDOBJ(gMainTBconnect);
   PISCOPE_BUILDOBJ(gMainTBlive);
   PISCOPE_BUILDOBJ(gMainTBpause);
   PISCOPE_BUILDOBJ(gMainTBplay);

   PISCOPE_BUILDOBJ(gTrigDialog);
   PISCOPE_BUILDOBJ(gTrigLabel);

   PISCOPE_BUILDOBJ(gTrgsDialog);
   PISCOPE_BUILDOBJ(gTrgsSamples);

   for (j=0; j<sizeof(gTrigSamplesText)/sizeof(gTrigSamplesText[0]); j++)
   {
      gtk_combo_box_text_insert_text
         (GTK_COMBO_BOX_TEXT(gTrgsSamples), j, gTrigSamplesText[j]);
   }

   gtk_combo_box_set_active(GTK_COMBO_BOX(gTrgsSamples), 0);

   for (i=0; i<PISCOPE_TRIGGERS; i++)
   {
      sprintf(buf, "trgs_on%d", i+1);

      gTrigInfo[i].onW = GTK_WIDGET(gtk_builder_get_object(builder, buf));

      sprintf(buf, "trgs_lab%d", i+1);

      gTrigInfo[i].labelW = GTK_WIDGET(gtk_builder_get_object(builder, buf));

      sprintf(buf, "trgs_when%d", i+1);

      gTrigInfo[i].whenW = GTK_WIDGET(gtk_builder_get_object(builder, buf));

      for (j=0; j<sizeof(gTrigWhenText)/sizeof(gTrigWhenText[0]); j++)
      {
         gtk_combo_box_text_insert_text
            (GTK_COMBO_BOX_TEXT(gTrigInfo[i].whenW), j, gTrigWhenText[j]);
      }

      gtk_combo_box_set_active(GTK_COMBO_BOX(gTrigInfo[i].whenW), 0);

   }

   for (i=PISCOPE_MIN_SPEED_IDX; i<=PISCOPE_MAX_SPEED_IDX; i++)
   {
      gtk_combo_box_text_insert_text
         (GTK_COMBO_BOX_TEXT(gCmdsPlayspeed), i, util_playSpeedStr(i));
   }

   gtk_combo_box_set_active
      (GTK_COMBO_BOX(gCmdsPlayspeed), PISCOPE_DEF_SPEED_IDX);

   for (i=0; i<PISCOPE_GPIOS; i++)
   {
      gGpioInfo[i].display = 1;

      sprintf(buf, "gpio%d", i);

      gGpioInfo[i].button =
         GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, buf));

      sprintf(buf, "trig%d", i);

      gTrigCombo[i] =
         GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, buf));

      for (j=0; j<sizeof(gTrigTypeText)/sizeof(gTrigTypeText[0]); j++)
      {
         gtk_combo_box_text_insert_text
            (gTrigCombo[i], j, gTrigTypeText[j]);
      }

      gtk_combo_box_set_active(GTK_COMBO_BOX(gTrigCombo[i]), 0);

   }

   pigpioSetAddr();

   /* set a minimum size */

   gtk_widget_set_size_request(gMainCosc, gCoscWidth, gCoscHeight);

   gtk_widget_set_events
   (
      gMainCosc,
      gtk_widget_get_events(gMainCosc) |
         GDK_POINTER_MOTION_MASK       |
         GDK_POINTER_MOTION_HINT_MASK  |
         GDK_BUTTON_PRESS_MASK
   );

   gtk_widget_set_events
   (
      gMainCvleg,
      gtk_widget_get_events(gMainCvleg) |
         GDK_BUTTON_PRESS_MASK
   );

   gtk_widget_set_events
   (
      gMainCbuf,
      gtk_widget_get_events(gMainCbuf) |
         GDK_BUTTON_PRESS_MASK
   );

   gtk_window_set_transient_for(GTK_WINDOW(gCmdsDialog), GTK_WINDOW(gMain));
   gtk_window_set_transient_for(GTK_WINDOW(gGpioDialog), GTK_WINDOW(gMain));
   gtk_window_set_transient_for(GTK_WINDOW(gTrigDialog), GTK_WINDOW(gMain));
   gtk_window_set_transient_for(GTK_WINDOW(gTrgsDialog), GTK_WINDOW(gMain));

   gtk_widget_show_all((GtkWidget*)gMain);

   gTimeSlotMicros = PISCOPE_MILLION / (gInputUpdateHz + (4*gOutputUpdateHz));

   gRefreshTicks = PISCOPE_MILLION / gOutputUpdateHz;

   g_timeout_add(1000/gInputUpdateHz, main_util_input, NULL);

   g_timeout_add(1000/gOutputUpdateHz, main_util_output, NULL);

   /* definitely done with the builder */

   g_object_unref(G_OBJECT(builder));

   pigpioConnect();

   gtk_main();

   /* free resources */

   if (gCoscSurface)  cairo_surface_destroy(gCoscSurface);
   if (gChlegSurface) cairo_surface_destroy(gChlegSurface);
   if (gCvlegSurface) cairo_surface_destroy(gCvlegSurface);
   if (gCsampSurface)  cairo_surface_destroy(gCsampSurface);
   if (gCmodeSurface) cairo_surface_destroy(gCmodeSurface);

   gtk_widget_destroy(GTK_WIDGET(gMain));

   gtk_widget_destroy(GTK_WIDGET(gCmdsDialog));

   return 0;
}
