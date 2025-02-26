/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"
#include "multi.h"
#include "pulses/afhds3.h"
#include "pulses/flysky.h"
#include "mixer_scheduler.h"
#include "io/multi_protolist.h"
#include "hal/module_port.h"

#if defined(LIBOPENUI)
  #include "libopenui.h"
#endif

#if !defined(SIMU)
  #include <FreeRTOS/include/FreeRTOS.h>
  #include <FreeRTOS/include/timers.h>
#endif

#include "spektrum.h"

#if defined(CROSSFIRE)
  #include "crossfire.h"
#endif

#if defined(GHOST)
  #include "ghost.h"
#endif

#if defined(MULTIMODULE)
  #include "hitec.h"
  #include "hott.h"
  #include "multi.h"
#endif

#if  defined(MULTIMODULE) || defined(PPM)
  #include "mlink.h"
#endif

#if defined(MULTIMODULE) || defined(AFHDS2) || defined(AFHDS3)
  #include "flysky_ibus.h"
#endif

uint8_t telemetryStreaming = 0;
uint8_t telemetryRxBuffer[TELEMETRY_RX_PACKET_SIZE];
uint8_t telemetryRxBufferCount = 0;

uint8_t telemetryState = TELEMETRY_INIT;

TelemetryData telemetryData;

uint8_t telemetryProtocol = 255;

#if defined(INTERNAL_MODULE_SERIAL_TELEMETRY)
static uint8_t intTelemetryRxBuffer[TELEMETRY_RX_PACKET_SIZE];
static uint8_t intTelemetryRxBufferCount;
#endif

static rxStatStruct rxStat;

uint8_t * getTelemetryRxBuffer(uint8_t moduleIdx)
{
#if defined(INTERNAL_MODULE_SERIAL_TELEMETRY)
  if (moduleIdx == INTERNAL_MODULE)
    return intTelemetryRxBuffer;
#endif
  return telemetryRxBuffer;
}

uint8_t &getTelemetryRxBufferCount(uint8_t moduleIdx)
{
#if defined(INTERNAL_MODULE_SERIAL_TELEMETRY)
  if (moduleIdx == INTERNAL_MODULE)
    return intTelemetryRxBufferCount;
#endif
  return telemetryRxBufferCount;
}

rxStatStruct *getRxStatLabels() {
  // default to RSSI/db notation
  rxStat.label = STR_RXSTAT_LABEL_RSSI;
  rxStat.unit  = STR_RXSTAT_UNIT_DBM;

  // Currently we can only display a single rx stat in settings/telemetry.
  // If both modules are used we choose the internal one
  // TODO: have to rx stat sections in settings/telemetry
  uint8_t moduleToUse = INTERNAL_MODULE;

  if(g_model.moduleData[INTERNAL_MODULE].type == MODULE_TYPE_NONE && 
     g_model.moduleData[EXTERNAL_MODULE].type != MODULE_TYPE_NONE) {
    moduleToUse = EXTERNAL_MODULE;
  }

  uint8_t moduleType = g_model.moduleData[moduleToUse].type;

  switch(moduleType) {
#if defined(MULTIMODULE)
    case MODULE_TYPE_MULTIMODULE: {
        uint8_t multiProtocol = g_model.moduleData[moduleToUse].multi.rfProtocol;

        if (multiProtocol == MODULE_SUBTYPE_MULTI_FS_AFHDS2A ||
            multiProtocol == MODULE_SUBTYPE_MULTI_HOTT ||
            multiProtocol == MODULE_SUBTYPE_MULTI_MLINK) {
          rxStat.label = STR_RXSTAT_LABEL_RQLY;
          rxStat.unit  = STR_RXSTAT_UNIT_PERCENT;
        }
      }
      break;
#endif
    case MODULE_TYPE_PPM:
      if(moduleState[moduleToUse].protocol == PROTOCOL_CHANNELS_PPM_MLINK) {
        rxStat.label = STR_RXSTAT_LABEL_RQLY;
        rxStat.unit  = STR_RXSTAT_UNIT_PERCENT;
      }
      break;

    case MODULE_TYPE_CROSSFIRE:
    case MODULE_TYPE_GHOST:
      rxStat.label = STR_RXSTAT_LABEL_RQLY;
      rxStat.unit  = STR_RXSTAT_UNIT_PERCENT;
      break;

#if defined (PCBNV14) && defined(AFHDS2)
    case MODULE_TYPE_FLYSKY_AFHDS2A:
        extern uint32_t NV14internalModuleFwVersion;

        if(moduleToUse == INTERNAL_MODULE) {
          if(NV14internalModuleFwVersion >= 0x1000E) {
            rxStat.label = STR_RXSTAT_LABEL_SIGNAL;
            rxStat.unit  = STR_RXSTAT_UNIT_NOUNIT ;
          }
        }
      break;
#endif      
  }

  return &rxStat;
}

// TODO: move to module port driver
//
// static int (*_telemetryGetByte)(void*, uint8_t*) = nullptr;
// static void* _telemetryGetByteCtx = nullptr;

// void telemetrySetGetByte(void* ctx, int (*fct)(void*, uint8_t*))
// {
//   _telemetryGetByte = nullptr;
//   _telemetryGetByteCtx = ctx;
//   _telemetryGetByte = fct;
// }

// static bool telemetryGetByte(uint8_t* data)
// {
//   auto _getByte = _telemetryGetByte;
//   auto _ctx = _telemetryGetByteCtx;

//   if (_getByte) {
//     return _getByte(_ctx, data);
//   }

//   // return sportGetByte(data);
//   return false;
// }

static void (*telemetryMirrorSendByte)(void*, uint8_t) = nullptr;
static void* telemetryMirrorSendByteCtx = nullptr;

void telemetrySetMirrorCb(void* ctx, void (*fct)(void*, uint8_t))
{
  telemetryMirrorSendByte = nullptr;
  telemetryMirrorSendByteCtx = ctx;
  telemetryMirrorSendByte = fct;
}

void telemetryMirrorSend(uint8_t data)
{
  auto _sendByte = telemetryMirrorSendByte;
  auto _ctx = telemetryMirrorSendByteCtx;

  if (_sendByte) {
    _sendByte(_ctx, data);
  }
}

#if !defined(SIMU)
static TimerHandle_t telemetryTimer = nullptr;
static StaticTimer_t telemetryTimerBuffer;

static void telemetryTimerCb(TimerHandle_t xTimer)
{
  (void)xTimer;

  DEBUG_TIMER_START(debugTimerTelemetryWakeup);
  telemetryWakeup();
  DEBUG_TIMER_STOP(debugTimerTelemetryWakeup);
}

void telemetryStart()
{
  if (!telemetryTimer) {
    telemetryTimer =
        xTimerCreateStatic("Telem", 2 / RTOS_MS_PER_TICK, pdTRUE, (void*)0,
                           telemetryTimerCb, &telemetryTimerBuffer);
  }

  if (telemetryTimer) {
    if( xTimerStart( telemetryTimer, 0 ) != pdPASS ) {
      /* The timer could not be set into the Active state. */
    }
  }
}

void telemetryStop()
{
  if (telemetryTimer) {
    if( xTimerStop( telemetryTimer, 5 / RTOS_MS_PER_TICK ) != pdPASS ) {
      /* The timer could not be stopped. */
    }
  }
}
#endif

inline bool isBadAntennaDetected()
{
  if (!isRasValueValid())
    return false;

  if (telemetryData.swrInternal.isFresh() &&
      telemetryData.swrInternal.value() > FRSKY_BAD_ANTENNA_THRESHOLD)
    return true;

  if (telemetryData.swrExternal.isFresh() &&
      telemetryData.swrExternal.value() > FRSKY_BAD_ANTENNA_THRESHOLD)
    return true;

  return false;
}

static inline void pollTelemetry(uint8_t module, const etx_proto_driver_t* drv, void* ctx)
{
  if (!drv || !drv->processData) return;

  auto mod_st = (etx_module_state_t*)ctx;
  auto serial_drv = modulePortGetSerialDrv(mod_st->rx);
  auto serial_ctx = modulePortGetCtx(mod_st->rx);

  if (!serial_drv  || !serial_ctx || !serial_drv->getByte)
    return;

  uint8_t* rxBuffer = getTelemetryRxBuffer(module);
  uint8_t& rxBufferCount = getTelemetryRxBufferCount(module);

  uint8_t data;
  if (serial_drv->getByte(serial_ctx, &data) > 0) {
    LOG_TELEMETRY_WRITE_START();
    do {
      telemetryMirrorSend(data);
      drv->processData(ctx, data, rxBuffer, &rxBufferCount);
      LOG_TELEMETRY_WRITE_BYTE(data);
    } while (serial_drv->getByte(serial_ctx, &data) > 0);
  }
}

// This can only be changed when the mixer is not
// running as the priority of the timer task is
// lower.
volatile uint8_t _telemetryIsPolling = false;

void telemetryWakeup()
{
  _telemetryIsPolling = true;
  for (uint8_t i = 0; i < MAX_MODULES; i++) {
    auto mod = pulsesGetModuleDriver(i);
    if (!mod) continue;
    pollTelemetry(i, mod->drv, mod->ctx);
  }
  _telemetryIsPolling = false;

  for (int i=0; i<MAX_TELEMETRY_SENSORS; i++) {
    const TelemetrySensor & sensor = g_model.telemetrySensors[i];
    if (sensor.type == TELEM_TYPE_CALCULATED) {
      telemetryItems[i].eval(sensor);
    }
  }

#if defined(VARIO)
  if (TELEMETRY_STREAMING() && !IS_FAI_ENABLED()) {
    varioWakeup();
  }
#endif

  static tmr10ms_t alarmsCheckTime = 0;
  #define SCHEDULE_NEXT_ALARMS_CHECK(seconds) alarmsCheckTime = get_tmr10ms() + (100*(seconds))
  if (int32_t(get_tmr10ms() - alarmsCheckTime) > 0) {

    SCHEDULE_NEXT_ALARMS_CHECK(1/*second*/);

    bool sensorLost = false;
    for (int i=0; i<MAX_TELEMETRY_SENSORS; i++) {
      if (isTelemetryFieldAvailable(i)) {
        TelemetryItem & item = telemetryItems[i];
        if (item.timeout == 0) {
          TelemetrySensor * sensor = & g_model.telemetrySensors[i];
          if (sensor->unit != UNIT_DATETIME) {
            item.setOld();
            sensorLost = true;
          }
        }
      }
    }

    if (sensorLost && TELEMETRY_STREAMING() && !g_model.disableTelemetryWarning) {
      audioEvent(AU_SENSOR_LOST);
    }

#if defined(PCBFRSKY)
    if (isBadAntennaDetected()) {
      AUDIO_RAS_RED();
      POPUP_WARNING_ON_UI_TASK(STR_WARNING, STR_ANTENNAPROBLEM);
      SCHEDULE_NEXT_ALARMS_CHECK(10/*seconds*/);
    }
#endif

    if (!g_model.disableTelemetryWarning) {
      if (TELEMETRY_STREAMING()) {
        if (TELEMETRY_RSSI() < g_model.rfAlarms.critical ) {
          AUDIO_RSSI_RED();
          SCHEDULE_NEXT_ALARMS_CHECK(10/*seconds*/);
        }
        else if (TELEMETRY_RSSI() < g_model.rfAlarms.warning ) {
          AUDIO_RSSI_ORANGE();
          SCHEDULE_NEXT_ALARMS_CHECK(10/*seconds*/);
        }
      }

      if (TELEMETRY_STREAMING()) {
        if (telemetryState == TELEMETRY_KO) {
          AUDIO_TELEMETRY_BACK();

#if defined(CROSSFIRE)
          // TODO: move to crossfire code
#if defined(HARDWARE_EXTERNAL_MODULE)
          if (isModuleCrossfire(EXTERNAL_MODULE)) {
            moduleState[EXTERNAL_MODULE].counter = CRSF_FRAME_MODELID;
          }
#endif

#if defined(HARDWARE_INTERNAL_MODULE)
          if (isModuleCrossfire(INTERNAL_MODULE)) {
            moduleState[INTERNAL_MODULE].counter = CRSF_FRAME_MODELID;
          }
#endif
#endif
        }
        telemetryState = TELEMETRY_OK;
      }
      else if (telemetryState == TELEMETRY_OK) {
        telemetryState = TELEMETRY_KO;
        if (!isModuleInBeepMode()) {
          AUDIO_TELEMETRY_LOST();
        }
      }
    }
  }
}

void telemetryInterrupt10ms()
{
  if (telemetryStreaming > 0) {
    bool tick160ms = (telemetryStreaming & 0x0F) == 0;
    for (int i=0; i<MAX_TELEMETRY_SENSORS; i++) {
      const TelemetrySensor & sensor = g_model.telemetrySensors[i];
      if (sensor.type == TELEM_TYPE_CALCULATED) {
        telemetryItems[i].per10ms(sensor);
      }
      if (tick160ms && telemetryItems[i].timeout > 0) {
        telemetryItems[i].timeout--;
      }
    }
    telemetryStreaming--;
  }
  else {
#if !defined(SIMU)
    telemetryData.rssi.reset();
#endif
    for (auto & telemetryItem: telemetryItems) {
      if (telemetryItem.isAvailable()) {
        telemetryItem.setOld();
      }
    }
  }
}

void telemetryReset()
{
  telemetryData.clear();

  for (auto & telemetryItem : telemetryItems) {
    telemetryItem.clear();
  }

  telemetryStreaming = 0; // reset counter only if valid telemetry packets are being detected

  telemetryState = TELEMETRY_INIT;
}

// we don't reset the telemetry here as we would also reset the consumption after model load
void telemetryInit(uint8_t protocol)
{
  telemetryProtocol = protocol;

//   if (protocol == PROTOCOL_TELEMETRY_FRSKY_D) {
//     telemetryPortInit(FRSKY_D_BAUDRATE, TELEMETRY_SERIAL_DEFAULT);
//   }
// #if defined(MULTIMODULE)
//   else if (protocol == PROTOCOL_TELEMETRY_MULTIMODULE) {
//     // The DIY Multi module always speaks 100000 baud regardless of the
//     // telemetry protocol in use
//     telemetryPortInit(MULTIMODULE_BAUDRATE, TELEMETRY_SERIAL_8E2);
// #if defined(LUA)
//     outputTelemetryBuffer.reset();
// #endif
//     telemetryPortSetDirectionInput();
//   } else if (protocol == PROTOCOL_TELEMETRY_SPEKTRUM) {
//     // Spektrum's own small race RX (SPM4648) uses 125000 8N1, use the same
//     // since there is no real standard
//     telemetryPortInit(125000, TELEMETRY_SERIAL_DEFAULT);
//   }
// #endif

// #if defined(GHOST)
//   else if (protocol == PROTOCOL_TELEMETRY_GHOST) {
//     telemetryPortInit(GHOST_BAUDRATE, TELEMETRY_SERIAL_DEFAULT);
// #if defined(LUA)
//     outputTelemetryBuffer.reset();
// #endif
//     telemetryPortSetDirectionOutput();
//   }
// #endif

// #if defined(AUX_SERIAL)
//   else if (protocol == PROTOCOL_TELEMETRY_FRSKY_D_SECONDARY) {
//     telemetryPortInit(0, TELEMETRY_SERIAL_DEFAULT);
//   }
// #endif
//   else if (protocol == PROTOCOL_TELEMETRY_DSMP) {
//     // soft serial
//     telemetryPortInvertedInit(115200);
//   } else {
//     telemetryPortInit(FRSKY_SPORT_BAUDRATE, TELEMETRY_SERIAL_WITHOUT_DMA);
// #if defined(LUA)
//     outputTelemetryBuffer.reset();
// #endif
//   }
}

#if defined(LOG_TELEMETRY) && !defined(SIMU)
extern FIL g_telemetryFile;
void logTelemetryWriteStart()
{
  static tmr10ms_t lastTime = 0;
  tmr10ms_t newTime = get_tmr10ms();
  if (lastTime != newTime) {
    struct gtm utm;
    gettime(&utm);
    f_printf(&g_telemetryFile, "\r\n%4d-%02d-%02d,%02d:%02d:%02d.%02d0:",
             utm.tm_year + TM_YEAR_BASE, utm.tm_mon + 1, utm.tm_mday,
             utm.tm_hour, utm.tm_min, utm.tm_sec, g_ms100);
    lastTime = newTime;
  }
}

void logTelemetryWriteByte(uint8_t data)
{
  f_printf(&g_telemetryFile, " %02X", data);
}
#endif

OutputTelemetryBuffer outputTelemetryBuffer __DMA;

#if defined(LUA)
Fifo<uint8_t, LUA_TELEMETRY_INPUT_FIFO_SIZE> * luaInputTelemetryFifo = NULL;
#endif

#if defined(HARDWARE_INTERNAL_MODULE)
static ModuleSyncStatus moduleSyncStatus[NUM_MODULES];

ModuleSyncStatus &getModuleSyncStatus(uint8_t moduleIdx)
{
  return moduleSyncStatus[moduleIdx];
}
#else
static ModuleSyncStatus moduleSyncStatus;

ModuleSyncStatus &getModuleSyncStatus(uint8_t moduleIdx)
{
  return moduleSyncStatus;
}
#endif

ModuleSyncStatus::ModuleSyncStatus()
{
  memset(this, 0, sizeof(ModuleSyncStatus));
}

void ModuleSyncStatus::update(uint16_t newRefreshRate, int16_t newInputLag)
{
  if (!newRefreshRate)
    return;
  
  if (newRefreshRate < MIN_REFRESH_RATE)
    newRefreshRate = newRefreshRate * (MIN_REFRESH_RATE / (newRefreshRate + 1));
  else if (newRefreshRate > MAX_REFRESH_RATE)
    newRefreshRate = MAX_REFRESH_RATE;

  refreshRate = newRefreshRate;
  inputLag    = newInputLag;
  currentLag  = newInputLag;
  lastUpdate  = get_tmr10ms();

#if 0
  TRACE("[SYNC] update rate = %dus; lag = %dus",refreshRate,currentLag);
#endif
}

void ModuleSyncStatus::invalidate() {
  //make invalid after use
  currentLag = 0;
}

uint16_t ModuleSyncStatus::getAdjustedRefreshRate()
{
  int16_t lag = currentLag;
  int32_t newRefreshRate = refreshRate;

  if (lag == 0) {
    return refreshRate;
  }
  
  newRefreshRate += lag;
  
  if (newRefreshRate < MIN_REFRESH_RATE) {
      newRefreshRate = MIN_REFRESH_RATE;
  }
  else if (newRefreshRate > MAX_REFRESH_RATE) {
    newRefreshRate = MAX_REFRESH_RATE;
  }

  currentLag -= newRefreshRate - refreshRate;
#if 0
  TRACE("[SYNC] mod rate = %dus; lag = %dus",newRefreshRate,currentLag);
#endif
  
  return (uint16_t)newRefreshRate;
}

void ModuleSyncStatus::getRefreshString(char * statusText)
{
  if (!isValid()) {
    return;
  }

  char * tmp = statusText;
#if defined(DEBUG)
  *tmp++ = 'L';
  tmp = strAppendSigned(tmp, inputLag, 5);
  tmp = strAppend(tmp, "R");
  tmp = strAppendUnsigned(tmp, refreshRate, 5);
#else
  tmp = strAppend(tmp, "Sync ");
  tmp = strAppendUnsigned(tmp, refreshRate);
#endif
  tmp = strAppend(tmp, "us");
}
