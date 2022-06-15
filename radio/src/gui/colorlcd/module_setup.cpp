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

#include "module_setup.h"
#include "opentx.h"

#include "form.h"
#include "choice.h"
#include "button.h"
#include "gridlayout.h"

#include "mixer_scheduler.h"
#include "multi_rfprotos.h"
#include "io/multi_protolist.h"
#include "pulses/flysky.h"
#include "bind_menu_d16.h"
#include "custom_failsafe.h"

#if defined(PXX2)
#include "access_settings.h"
#endif

#define SET_DIRTY()     storageDirty(EE_MODEL)

class ModuleWindow : public FormGroup
{
 public:
  ModuleWindow(FormWindow *parent, const rect_t &rect, uint8_t moduleIdx);
  void update();

 protected:
  uint8_t moduleIdx;
  bool hasFailsafe = false;

  Choice *rfChoice = nullptr;
  TextButton *bindButton = nullptr;
  TextButton *rangeButton = nullptr;
  TextButton *registerButton = nullptr;
  Choice *failSafeChoice = nullptr;

  void addChannelRange(FormGridLayout &grid);
  void startRSSIDialog(std::function<void()> closeHandler = nullptr);
  void checkEvents() override;
};

ModuleWindow::ModuleWindow(FormWindow *parent, const rect_t &rect,
                           uint8_t moduleIdx) :
    FormGroup(parent, rect, FORWARD_SCROLL | NO_FOCUS /*| FORM_FORWARD_FOCUS*/),
    moduleIdx(moduleIdx)
{
  update();
}

void ModuleWindow::addChannelRange(FormGridLayout &grid)
{
  new StaticText(this, grid.getLabelSlot(), STR_CHANNELRANGE, 0,
                 COLOR_THEME_PRIMARY1);
  auto channelStart = new NumberEdit(
      this, grid.getFieldSlot(2, 0), 1,
      MAX_OUTPUT_CHANNELS - sentModuleChannels(moduleIdx) + 1,
      GET_DEFAULT(1 + g_model.moduleData[moduleIdx].channelsStart));
  auto channelEnd = new NumberEdit(
      this, grid.getFieldSlot(2, 1),
      g_model.moduleData[moduleIdx].channelsStart +
          minModuleChannels(moduleIdx),
      min<int8_t>(MAX_OUTPUT_CHANNELS,
                  g_model.moduleData[moduleIdx].channelsStart +
                      maxModuleChannels(moduleIdx)),
      GET_DEFAULT(g_model.moduleData[moduleIdx].channelsStart + 8 +
                  g_model.moduleData[moduleIdx].channelsCount));
  if (isModulePXX2(moduleIdx)) {
    channelEnd->setAvailableHandler(
        [=](int value) { return isPxx2IsrmChannelsCountAllowed(value - 8); });
  }
  channelStart->setPrefix(STR_CH);
  channelEnd->setPrefix(STR_CH);
  channelStart->setSetValueHandler([=](int32_t newValue) {
    g_model.moduleData[moduleIdx].channelsStart = newValue - 1;
    SET_DIRTY();
    channelEnd->setMin(g_model.moduleData[moduleIdx].channelsStart +
                       minModuleChannels(moduleIdx));
    channelEnd->setMax(min<int8_t>(MAX_OUTPUT_CHANNELS,
                                   g_model.moduleData[moduleIdx].channelsStart +
                                       maxModuleChannels(moduleIdx)));
    channelEnd->invalidate();
  });
  channelEnd->setSetValueHandler([=](int32_t newValue) {
    g_model.moduleData[moduleIdx].channelsCount =
        newValue - g_model.moduleData[moduleIdx].channelsStart - 8;
    SET_DIRTY();
    channelStart->setMax(MAX_OUTPUT_CHANNELS - sentModuleChannels(moduleIdx) +
                         1);
  });
  channelEnd->enable(minModuleChannels(moduleIdx) <
                     maxModuleChannels(moduleIdx));
  if (channelEnd->getValue() > channelEnd->getMax())
    channelEnd->setValue(channelEnd->getMax());
}

void ModuleWindow::update()
{
  FormGridLayout grid;
  clear();

  if (moduleIdx == INTERNAL_MODULE && isModuleCrossfire(moduleIdx)) {
    char buf[6];
    new StaticText(this, grid.getFieldSlot(2, 1),
                   getStringAtIndex(buf, STR_CRSF_BAUDRATE,
                                    CROSSFIRE_STORE_TO_INDEX(
                                        g_eeGeneral.internalModuleBaudrate)),
                   0, COLOR_THEME_PRIMARY1);
  }

  // Module parameters
  if (moduleIdx == EXTERNAL_MODULE && isModuleCrossfire(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_BAUDRATE, 0,
                   COLOR_THEME_PRIMARY1);
    new Choice(
        this, grid.getFieldSlot(1, 0), STR_CRSF_BAUDRATE, 0,
        CROSSFIRE_MAX_INTERNAL_BAUDRATE,
        [=]() -> int {
          return CROSSFIRE_STORE_TO_INDEX(
              g_model.moduleData[moduleIdx].crsf.telemetryBaudrate);
        },
        [=](int newValue) {
          g_model.moduleData[moduleIdx].crsf.telemetryBaudrate =
              CROSSFIRE_INDEX_TO_STORE(newValue);
          SET_DIRTY();
          restartModule(moduleIdx);
        });
    grid.nextLine();
  }
  if (isModuleCrossfire(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_STATUS, 0,
                   COLOR_THEME_PRIMARY1);
    new DynamicText(this, grid.getFieldSlot(), [=] {
      char msg[64] = "";
      sprintf(msg, "%d Hz %lu Err", 1000000 / getMixerSchedulerPeriod(),
              telemetryErrors);
      return std::string(msg);
    });
    grid.nextLine();
  }

#if defined(AFHDS2) || defined(AFHDS3)
  if (isModuleFlySky(moduleIdx)) {

    // RX options:
    new StaticText(this, grid.getLabelSlot(), STR_OPTIONS, 0,
                   COLOR_THEME_PRIMARY1);

#if defined(AFHDS2)
    if (isModuleAFHDS2A(moduleIdx)) {
      // PPM / PWM
      new Choice(
          this, grid.getFieldSlot(2, 0), STR_FLYSKY_PULSE_PROTO, 0, 1,
          [=]() { return g_model.moduleData[moduleIdx].flysky.mode >> 1; },
          [=](int v) {
            g_model.moduleData[moduleIdx].flysky.mode =
                (g_model.moduleData[moduleIdx].flysky.mode & 1) |
                ((v & 1) << 1);
          });
      // SBUS / iBUS
      new Choice(
          this, grid.getFieldSlot(2, 1), STR_FLYSKY_SERIAL_PROTO, 0, 1,
          [=]() { return g_model.moduleData[moduleIdx].flysky.mode & 1; },
          [=](int v) {
            g_model.moduleData[moduleIdx].flysky.mode =
                (g_model.moduleData[moduleIdx].flysky.mode & 2) | (v & 1);
          });
      grid.nextLine();
    }
#endif
#if defined(AFHDS3)
    if (isModuleAFHDS3(moduleIdx)) {
      // PPM / PWM
      new Choice(
          this, grid.getFieldSlot(2, 0), STR_FLYSKY_PULSE_PROTO, 0, 1,
          [=]() { return g_model.moduleData[moduleIdx].afhds3.mode >> 1; },
          [=](int v) {
            g_model.moduleData[moduleIdx].afhds3.mode =
                (g_model.moduleData[moduleIdx].afhds3.mode & 1) |
                ((v & 1) << 1);
          });
      // SBUS / iBUS
      new Choice(
          this, grid.getFieldSlot(2, 1), STR_FLYSKY_SERIAL_PROTO, 0, 1,
          [=]() { return g_model.moduleData[moduleIdx].afhds3.mode & 1; },
          [=](int v) {
            g_model.moduleData[moduleIdx].afhds3.mode =
                (g_model.moduleData[moduleIdx].afhds3.mode & 2) | (v & 1);
          });
      grid.nextLine();

      // TYPE
      new StaticText(this, grid.getLabelSlot(), STR_TYPE, 0,
                     COLOR_THEME_PRIMARY1);

      // This is chosen when binding (menu? see stdlcd/model_setup_afhds3.cpp)
      new StaticText(this, grid.getFieldSlot(),
                     g_model.moduleData[moduleIdx].afhds3.telemetry
                         ? STR_AFHDS3_ONE_TO_ONE_TELEMETRY
                         : TR_AFHDS3_ONE_TO_MANY,
                     0, COLOR_THEME_PRIMARY1);
      grid.nextLine();

      // Status
      new StaticText(this, grid.getLabelSlot(), STR_MODULE_STATUS, 0,
                     COLOR_THEME_PRIMARY1);
      new DynamicText(this, grid.getFieldSlot(), [=] {
        char msg[64] = "";
        getModuleStatusString(moduleIdx, msg);
        return std::string(msg);
      });
      grid.nextLine();

      // Power source
      new StaticText(this, grid.getLabelSlot(), STR_AFHDS3_POWER_SOURCE, 0,
                     COLOR_THEME_PRIMARY1);
      new DynamicText(this, grid.getFieldSlot(), [=] {
        char msg[64] = "";
        getModuleSyncStatusString(moduleIdx, msg);
        return std::string(msg);
      });
      grid.nextLine();

      // RX Freq
      new StaticText(this, grid.getLabelSlot(), STR_AFHDS3_RX_FREQ, 0,
                     COLOR_THEME_PRIMARY1);
      auto edit = new NumberEdit(
          this, grid.getFieldSlot(2, 0), MIN_FREQ, MAX_FREQ,
          GET_DEFAULT(g_model.moduleData[moduleIdx].afhds3.rxFreq()));
      edit->setSetValueHandler([=](int32_t newValue) {
        g_model.moduleData[moduleIdx].afhds3.setRxFreq((uint16_t)newValue);
      });
      edit->setSuffix(STR_HZ);
      grid.nextLine();

      // Module actual power
      new StaticText(this, grid.getLabelSlot(), STR_AFHDS3_ACTUAL_POWER, 0,
                     COLOR_THEME_PRIMARY1);
      new DynamicText(this, grid.getFieldSlot(), [=] {
        char msg[64] = "";
        getStringAtIndex(msg, STR_AFHDS3_POWERS,
                         actualAfhdsRunPower(moduleIdx));
        return std::string(msg);
      });
      grid.nextLine();

      // Module power
      new StaticText(this, grid.getLabelSlot(), STR_RF_POWER, 0,
                     COLOR_THEME_PRIMARY1);
      new Choice(
          this, grid.getFieldSlot(2, 0), STR_AFHDS3_POWERS,
          afhds3::RUN_POWER::RUN_POWER_FIRST, afhds3::RUN_POWER::RUN_POWER_LAST,
          GET_SET_DEFAULT(g_model.moduleData[moduleIdx].afhds3.runPower));
      grid.nextLine();
    }
#endif
  }
#endif
#if defined(MULTIMODULE)
  else if (isModuleMultimodule(moduleIdx)) {

    // TODO: needs to be placed differently
    // MultiModuleStatus &status = getMultiModuleStatus(moduleIdx);
    // if (status.protocolName[0] && status.isValid()) {
    //   new StaticText(this, grid.getFieldSlot(2, 1), status.protocolName, 0,
    //                  COLOR_THEME_PRIMARY1);
    //   grid.nextLine();
    // }

    Choice *mmSubProto = nullptr;
    new StaticText(this, grid.getLabelSlot(), STR_RF_PROTOCOL, 0,
                   COLOR_THEME_PRIMARY1);

    auto *rfProto = MultiRfProtocols::instance(moduleIdx)->getProto(
        g_model.moduleData[moduleIdx].multi.rfProtocol);

    if (rfProto && !rfProto->subProtos.empty()) {
      // Subtype (D16, DSMX,...)
      mmSubProto = new Choice(
          this, grid.getFieldSlot(), rfProto->subProtos, 0,
          rfProto->subProtos.size() - 1,
          [=]() { return g_model.moduleData[moduleIdx].subType; },
          [=](int16_t newValue) {
            g_model.moduleData[moduleIdx].subType = newValue;
            resetMultiProtocolsOptions(moduleIdx);
            SET_DIRTY();
            // update();
            // if (mmSubProto != nullptr) mmSubProto->setFocus(SET_FOCUS_DEFAULT);
          });
    }
    grid.nextLine();

    // Multimodule status
    new StaticText(this, grid.getLabelSlot(), STR_MODULE_STATUS, 0,
                   COLOR_THEME_PRIMARY1);
    new DynamicText(
        this, grid.getFieldSlot(),
        [=] {
          char msg[64] = "";
          getModuleStatusString(moduleIdx, msg);
          return std::string(msg);
        },
        COLOR_THEME_PRIMARY1);
    grid.nextLine();

    const uint8_t multi_proto = g_model.moduleData[moduleIdx].multi.rfProtocol;
    if (rfProto) {
      // Multi optional feature row
      const char *title = rfProto->getOptionStr();
      if (title != nullptr) {
        new StaticText(this, grid.getLabelSlot(), title, 0,
                       COLOR_THEME_PRIMARY1);

        int8_t min, max;
        getMultiOptionValues(multi_proto, min, max);

        if (title == STR_MULTI_RFPOWER) {
          new Choice(
              this, grid.getFieldSlot(2, 0), STR_MULTI_POWER, 0, 15,
              GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.optionValue));
        } else if (title == STR_MULTI_TELEMETRY) {
          new Choice(
              this, grid.getFieldSlot(2, 0), STR_MULTI_TELEMETRY_MODE, min, max,
              GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.optionValue));
        } else if (title == STR_MULTI_WBUS) {
          new Choice(
              this, grid.getFieldSlot(2, 0), STR_MULTI_WBUS_MODE, 0, 1,
              GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.optionValue));
        } else if (multi_proto == MODULE_SUBTYPE_MULTI_FS_AFHDS2A) {
          auto edit = new NumberEdit(
              this, grid.getFieldSlot(2, 0), 50, 400,
              GET_DEFAULT(50 +
                          5 * g_model.moduleData[moduleIdx].multi.optionValue),
              SET_VALUE(g_model.moduleData[moduleIdx].multi.optionValue,
                        (newValue - 50) / 5));
          edit->setStep(5);
        } else if (multi_proto == MODULE_SUBTYPE_MULTI_DSM2) {
          new CheckBox(
              this, grid.getFieldSlot(2, 0),
              [=]() {
                return g_model.moduleData[moduleIdx].multi.optionValue & 0x01;
              },
              [=](int16_t newValue) {
                g_model.moduleData[moduleIdx].multi.optionValue =
                    (g_model.moduleData[moduleIdx].multi.optionValue & 0xFE) +
                    newValue;
              });
        } else {
          if (min == 0 && max == 1) {
            new CheckBox(this, grid.getFieldSlot(2, 0),
                         GET_SET_DEFAULT(
                             g_model.moduleData[moduleIdx].multi.optionValue));
          } else {
            new NumberEdit(
                this, grid.getFieldSlot(2, 0), -128, 127,
                GET_SET_DEFAULT(
                    g_model.moduleData[moduleIdx].multi.optionValue));

            // Show RSSI next to RF Freq Fine Tune
            if (title == STR_MULTI_RFTUNE) {
              new DynamicNumber<int>(
                  this, grid.getFieldSlot(2, 1),
                  [] { return (int)TELEMETRY_RSSI(); }, 0, "RSSI: ", " db");
            }
          }
        }
        grid.nextLine();
      }
    }

    if (multi_proto == MODULE_SUBTYPE_MULTI_DSM2) {
      const char *servoRates[] = {"22ms", "11ms"};

      new StaticText(this, grid.getLabelSlot(), STR_MULTI_SERVOFREQ, 0,
                     COLOR_THEME_PRIMARY1);
      new Choice(
          this, grid.getFieldSlot(), servoRates, 0, 1,
          [=]() {
            return (g_model.moduleData[moduleIdx].multi.optionValue & 0x02) >>
                   1;
          },
          [=](int16_t newValue) {
            g_model.moduleData[moduleIdx].multi.optionValue =
                (g_model.moduleData[moduleIdx].multi.optionValue & 0xFD) +
                (newValue << 1);
          });
    } else {
      // Bind on power up
      new StaticText(this, grid.getLabelSlot(), STR_MULTI_AUTOBIND, 0,
                     COLOR_THEME_PRIMARY1);
      new CheckBox(
          this, grid.getFieldSlot(),
          GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.autoBindMode));
    }
    grid.nextLine();

    // Low power mode
    new StaticText(this, grid.getLabelSlot(), STR_MULTI_LOWPOWER, 0,
                   COLOR_THEME_PRIMARY1);
    new CheckBox(
        this, grid.getFieldSlot(),
        GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.lowPowerMode));
    grid.nextLine();

    // Disable telemetry
    new StaticText(this, grid.getLabelSlot(), STR_DISABLE_TELEM, 0,
                   COLOR_THEME_PRIMARY1);
    new CheckBox(
        this, grid.getFieldSlot(),
        GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.disableTelemetry));
    grid.nextLine();

    if (rfProto && rfProto->supportsDisableMapping()) {
      // Disable channel mapping
      new StaticText(this, grid.getLabelSlot(), STR_DISABLE_CH_MAP, 0,
                     COLOR_THEME_PRIMARY1);
      new CheckBox(
          this, grid.getFieldSlot(),
          GET_SET_DEFAULT(g_model.moduleData[moduleIdx].multi.disableMapping));
      grid.nextLine();
    }
  }
#endif

  // Channel Range
  if (g_model.moduleData[moduleIdx].type != MODULE_TYPE_NONE) {
    addChannelRange(grid);  // TODO XJT2 should only set channel count of
                            // 8/16/24
    grid.nextLine();
  }

  // PPM modules
  if (isModulePPM(moduleIdx)) {
    // PPM frame
    new StaticText(this, grid.getLabelSlot(), STR_PPMFRAME, 0,
                   COLOR_THEME_PRIMARY1);

    // PPM frame length
    auto edit = new NumberEdit(
        this, grid.getFieldSlot(3, 0), 125, 35 * PPM_STEP_SIZE + PPM_DEF_PERIOD,
        GET_DEFAULT(g_model.moduleData[moduleIdx].ppm.frameLength *
                        PPM_STEP_SIZE +
                    PPM_DEF_PERIOD),
        SET_VALUE(g_model.moduleData[moduleIdx].ppm.frameLength,
                  (newValue - PPM_DEF_PERIOD) / PPM_STEP_SIZE),
        0, PREC1);
    edit->setStep(PPM_STEP_SIZE);
    edit->setSuffix(STR_MS);

    // PPM frame delay
    edit = new NumberEdit(
        this, grid.getFieldSlot(3, 1), 100, 800,
        GET_DEFAULT(g_model.moduleData[moduleIdx].ppm.delay * 50 + 300),
        SET_VALUE(g_model.moduleData[moduleIdx].ppm.delay,
                  (newValue - 300) / 50));
    edit->setStep(50);
    edit->setSuffix(STR_US);

    // PPM Polarity
    new Choice(this, grid.getFieldSlot(3, 2), STR_PPM_POL, 0, 1,
               GET_SET_DEFAULT(g_model.moduleData[moduleIdx].ppm.pulsePol));
    grid.nextLine();
  }

  // Module parameters

  // Bind and Range buttons
  if (!isModuleRFAccess(moduleIdx) && (isModuleModelIndexAvailable(moduleIdx) ||
                                       isModuleBindRangeAvailable(moduleIdx))) {
    uint8_t thirdColumn = 0;
    new StaticText(this, grid.getLabelSlot(), STR_RECEIVER, 0,
                   COLOR_THEME_PRIMARY1);

    // Model index
    if (isModuleModelIndexAvailable(moduleIdx)) {
      thirdColumn++;
      new NumberEdit(this, grid.getFieldSlot(3, 0), 0, getMaxRxNum(moduleIdx),
                     GET_DEFAULT(g_model.header.modelId[moduleIdx]),
                     [=](int32_t newValue) {
                       if (newValue != g_model.header.modelId[moduleIdx]) {
                         g_model.header.modelId[moduleIdx] = newValue;
                         if (isModuleCrossfire(moduleIdx)) {
                           moduleState[moduleIdx].counter = CRSF_FRAME_MODELID;
                         }
                         SET_DIRTY();
                       }
                     });
    }

    if (isModuleBindRangeAvailable(moduleIdx)) {
      bindButton = new TextButton(
          this, grid.getFieldSlot(2 + thirdColumn, 0 + thirdColumn),
          STR_MODULE_BIND);
      bindButton->setPressHandler([=]() -> uint8_t {
        if (moduleState[moduleIdx].mode == MODULE_MODE_RANGECHECK) {
          if (rangeButton) rangeButton->check(false);
        }
        if (moduleState[moduleIdx].mode == MODULE_MODE_BIND) {
          moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
#if defined(AFHDS2)
          if (isModuleFlySky(moduleIdx)) resetPulsesAFHDS2();
#endif
          if (isModuleDSMP(moduleIdx)) restartModule(moduleIdx);
          return 0;
        } else {
          if (isModuleR9MNonAccess(moduleIdx) || isModuleD16(moduleIdx) ||
              IS_R9_MULTI(moduleIdx)) {
            new BindChoiceMenu(
                this, moduleIdx,
                [=]() {
#if defined(MULTIMODULE)
                  if (isModuleMultimodule(moduleIdx)) {
                    setMultiBindStatus(moduleIdx, MULTI_BIND_INITIATED);
                  }
#endif
                  moduleState[moduleIdx].mode = MODULE_MODE_BIND;
                  bindButton->check(true);
                },
                [=]() {
                  moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
                  bindButton->check(false);
                });

            return 0;
          }
#if defined(MULTIMODULE)
          if (isModuleMultimodule(moduleIdx)) {
            setMultiBindStatus(moduleIdx, MULTI_BIND_INITIATED);
          }
#endif
          moduleState[moduleIdx].mode = MODULE_MODE_BIND;
#if defined(AFHDS2)
          if (isModuleFlySky(moduleIdx)) {
            resetPulsesAFHDS2();
          }
#endif
          return 1;
        }
        return 0;
      });
      bindButton->setCheckHandler([=]() {
        if (moduleState[moduleIdx].mode != MODULE_MODE_BIND) {
          if (bindButton->checked()) {
            bindButton->check(false);
            this->invalidate();
          }
        }
#if defined(MULTIMODULE)
        if (isModuleMultimodule(moduleIdx) &&
            getMultiBindStatus(moduleIdx) == MULTI_BIND_FINISHED) {
          setMultiBindStatus(moduleIdx, MULTI_BIND_NONE);
          moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
          bindButton->check(false);
        }
#endif
      });

      if (isModuleRangeAvailable(moduleIdx)) {
        rangeButton = new TextButton(
            this, grid.getFieldSlot(2 + thirdColumn, 1 + thirdColumn),
            STR_MODULE_RANGE);
        rangeButton->setPressHandler([=]() -> uint8_t {
          if (moduleState[moduleIdx].mode == MODULE_MODE_BIND) {
            bindButton->check(false);
            moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
          }
          if (moduleState[moduleIdx].mode == MODULE_MODE_RANGECHECK) {
            moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
            return 0;
          } else {
            moduleState[moduleIdx].mode = MODULE_MODE_RANGECHECK;
#if defined(AFHDS2)
            if (isModuleFlySky(moduleIdx)) {
              resetPulsesAFHDS2();
            }
#endif
            startRSSIDialog([=]() {
#if defined(AFHDS2)
              if (isModuleFlySky(moduleIdx)) {
                resetPulsesAFHDS2();
              }
#endif
            });
            return 1;
          }
        });
      }
    }

    grid.nextLine();
  }

#if defined(AFHDS2) && defined(PCBNV14)
  if (isModuleAFHDS2A(moduleIdx) && getNV14RfFwVersion() >= 0x1000E) {
    new StaticText(this, grid.getLabelSlot(), STR_MULTI_RFPOWER);
    new Choice(this, grid.getFieldSlot(),
               "\007"
               "Default"
               "High",
               0, 1, GET_DEFAULT(g_model.moduleData[moduleIdx].flysky.rfPower),
               [=](int32_t newValue) -> void {
                 g_model.moduleData[moduleIdx].flysky.rfPower = newValue;
                 resetPulsesAFHDS2();
               });
    grid.nextLine();
  }
#endif

  // Failsafe
  if (isModuleFailsafeAvailable(moduleIdx)) {
    hasFailsafe = true;
    new StaticText(this, grid.getLabelSlot(), STR_FAILSAFE, 0,
                   COLOR_THEME_PRIMARY1);
    failSafeChoice = new Choice(
        this, grid.getFieldSlot(2, 0), STR_VFAILSAFE, 0, FAILSAFE_LAST,
        GET_DEFAULT(g_model.moduleData[moduleIdx].failsafeMode),
        [=](int32_t newValue) {
          g_model.moduleData[moduleIdx].failsafeMode = newValue;
          SET_DIRTY();
          // update();
          // failSafeChoice->setFocus(SET_FOCUS_DEFAULT);
        });
    if (g_model.moduleData[moduleIdx].failsafeMode == FAILSAFE_CUSTOM) {
      new TextButton(this, grid.getFieldSlot(2, 1), STR_SET, [=]() -> uint8_t {
        new FailSafePage(moduleIdx);
        return 1;
      });
    }
    grid.nextLine();
  }

#if defined(PXX2)
  // Register and Range buttons
  if (isModuleRFAccess(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_MODULE, 0,
                   COLOR_THEME_PRIMARY1);
    registerButton =
        new TextButton(this, grid.getFieldSlot(2, 0), STR_REGISTER);
    registerButton->setPressHandler([=]() -> uint8_t {
      new RegisterDialog(this, moduleIdx);
      return 0;
    });

    rangeButton =
        new TextButton(this, grid.getFieldSlot(2, 1), STR_MODULE_RANGE);
    rangeButton->setPressHandler([=]() -> uint8_t {
      if (moduleState[moduleIdx].mode == MODULE_MODE_RANGECHECK) {
        moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
        return 0;
      } else {
        moduleState[moduleIdx].mode = MODULE_MODE_RANGECHECK;
        startRSSIDialog();
        return 1;
      }
    });

    grid.nextLine();

    new StaticText(this, grid.getLabelSlot(), TR_OPTIONS, 0,
                   COLOR_THEME_PRIMARY1);
    auto options = new TextButton(this, grid.getFieldSlot(2, 0), TR_SET);
    options->setPressHandler([=]() {
      new ModuleOptions(this, moduleIdx);
      return 0;
    });
    grid.nextLine();
  }
#endif

  // R9M Power
  if (isModuleR9M_FCC_VARIANT(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_RF_POWER, 0,
                   COLOR_THEME_PRIMARY1);
    new Choice(this, grid.getFieldSlot(), STR_R9M_FCC_POWER_VALUES, 0,
               R9M_FCC_POWER_MAX,
               GET_SET_DEFAULT(g_model.moduleData[moduleIdx].pxx.power));
  }

  if (isModuleR9M_LBT(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_RF_POWER, 0,
                   COLOR_THEME_PRIMARY1);
    new Choice(this, grid.getFieldSlot(), STR_R9M_LBT_POWER_VALUES, 0,
               R9M_LBT_POWER_MAX,
               GET_DEFAULT(min<uint8_t>(g_model.moduleData[moduleIdx].pxx.power,
                                        R9M_LBT_POWER_MAX)),
               SET_DEFAULT(g_model.moduleData[moduleIdx].pxx.power));
  }
#if defined(PXX2)
  // Receivers
  if (isModuleRFAccess(moduleIdx)) {
    for (uint8_t receiverIdx = 0; receiverIdx < PXX2_MAX_RECEIVERS_PER_MODULE;
         receiverIdx++) {
      char label[] = TR_RECEIVER " X";
      label[sizeof(label) - 2] = '1' + receiverIdx;
      new StaticText(this, grid.getLabelSlot(), label, 0,
                     COLOR_THEME_PRIMARY1);
      new ReceiverButton(this, grid.getFieldSlot(2, 0), moduleIdx, receiverIdx);
      grid.nextLine();
    }
  }
#endif
  // SBUS refresh rate
  if (isModuleSBUS(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), STR_REFRESHRATE, 0,
                   COLOR_THEME_PRIMARY1);
    auto edit = new NumberEdit(
        this, grid.getFieldSlot(2, 0), SBUS_MIN_PERIOD, SBUS_MAX_PERIOD,
        GET_DEFAULT((int16_t)g_model.moduleData[moduleIdx].sbus.refreshRate *
                        SBUS_STEPSIZE +
                    SBUS_DEF_PERIOD),
        SET_VALUE(g_model.moduleData[moduleIdx].sbus.refreshRate,
                  (newValue - SBUS_DEF_PERIOD) / SBUS_STEPSIZE),
        0, PREC1);
    edit->setSuffix(STR_MS);
    edit->setStep(SBUS_STEPSIZE);
    new Choice(this, grid.getFieldSlot(2, 1), STR_SBUS_INVERSION_VALUES, 0, 1,
               GET_SET_DEFAULT(g_model.moduleData[moduleIdx].sbus.noninverted));
#if defined(RADIO_TX16S)
    grid.nextLine();
    new StaticText(this, grid.getFieldSlot(1, 0), STR_WARN_5VOLTS, 0,
                   COLOR_THEME_PRIMARY1);
#endif
    grid.nextLine();
  }

  if (isModuleGhost(moduleIdx)) {
    new StaticText(this, grid.getLabelSlot(), "Raw 12 bits", 0,
                   COLOR_THEME_PRIMARY1);
    new CheckBox(
        this, grid.getFieldSlot(),
        GET_SET_DEFAULT(g_model.moduleData[moduleIdx].ghost.raw12bits));
  }
}

#if defined(PCBNV14)
#define SIGNAL_POSTFIX
#define SIGNAL_MESSAGE "SGNL"
#else
#define SIGNAL_POSTFIX " db"
#define SIGNAL_MESSAGE "RSSI"
#endif

void ModuleWindow::startRSSIDialog(std::function<void()> closeHandler)
{
  auto rssiDialog = new DynamicMessageDialog(
      parent, "Range Test",
      [=]() {
        return std::to_string((int)TELEMETRY_RSSI()) +
               std::string(SIGNAL_POSTFIX);
      },
      SIGNAL_MESSAGE, 50,
      COLOR_THEME_SECONDARY1 | CENTERED | FONT(BOLD) | FONT(XL));

  rssiDialog->setCloseHandler([this, closeHandler]() {
    rangeButton->check(false);
    moduleState[moduleIdx].mode = MODULE_MODE_NORMAL;
    if (closeHandler) closeHandler();
  });
}

void ModuleWindow::checkEvents()
{
  if (isModuleFailsafeAvailable(moduleIdx) != hasFailsafe && rfChoice &&
      !rfChoice->isEditMode()) {
    hasFailsafe = isModuleFailsafeAvailable(moduleIdx);
    update();
  }

  FormGroup::checkEvents();
}

class ModuleSubTypeChoice: public Choice
{
  uint8_t moduleIdx;

public:
  ModuleSubTypeChoice(Window* parent, const rect_t &rect, uint8_t moduleIdx);
  void update();
  void openMenu() override;
};

ModuleSubTypeChoice::ModuleSubTypeChoice(Window *parent, const rect_t &rect,
                                         uint8_t moduleIdx) :
    Choice(parent, rect, 0, 0, nullptr), moduleIdx(moduleIdx)
{
  ModuleData *md = &g_model.moduleData[moduleIdx];
  setGetValueHandler(GET_DEFAULT(md->subType));
  update();
}

void ModuleSubTypeChoice::update()
{
  ModuleData* md = &g_model.moduleData[moduleIdx];

  if (isModuleXJT(moduleIdx)) {

    setMin(MODULE_SUBTYPE_PXX1_OFF);
    setMax(MODULE_SUBTYPE_PXX1_LAST);
    setValues(STR_XJT_ACCST_RF_PROTOCOLS);
    setGetValueHandler(GET_DEFAULT(md->subType));
    setSetValueHandler([=](int32_t newValue) {
      md->subType = newValue;
      md->channelsStart = 0;
      md->channelsCount = defaultModuleChannels_M8(moduleIdx);
      SET_DIRTY();
    });
    setAvailableHandler(
        [](int index) { return index != MODULE_SUBTYPE_PXX1_OFF; });
  }
  else if (isModuleDSM2(moduleIdx)) {
    setMin(DSM2_PROTO_LP45);
    setMax(DSM2_PROTO_DSMX);
    setValues(STR_DSM_PROTOCOLS);
    setGetValueHandler(GET_DEFAULT(md->subType));
    setSetValueHandler(SET_DEFAULT(md->subType));
    setAvailableHandler(nullptr);
  }
  else if (isModuleR9M(moduleIdx)) {
    setMin(MODULE_SUBTYPE_R9M_FCC);
    setMax(MODULE_SUBTYPE_R9M_LAST);
    setValues(STR_R9M_REGION);
    setGetValueHandler(GET_DEFAULT(md->subType));
    setSetValueHandler(SET_DEFAULT(md->subType));
    setAvailableHandler(nullptr);    
  }
#if defined(PXX2)
  else if (isModulePXX2(moduleIdx)) {
    setMin(MODULE_SUBTYPE_ISRM_PXX2_ACCESS);
    setMax(MODULE_SUBTYPE_ISRM_PXX2_ACCST_D16);
    setValues(STR_ISRM_RF_PROTOCOLS);
    setGetValueHandler(GET_DEFAULT(md->subType));
    setSetValueHandler(SET_DEFAULT(md->subType));
    setAvailableHandler(nullptr);    
  }
#endif
#if defined(AFHDS2) || defined(AFHDS3)
  else if (isModuleFlySky(moduleIdx)) {
    setMin(0);
    setMax(FLYSKY_SUBTYPE_AFHDS2A);
    setValues(STR_FLYSKY_PROTOCOLS);
    setGetValueHandler(GET_DEFAULT(md->subType));
    setSetValueHandler(SET_DEFAULT(md->subType));

    if (moduleIdx == INTERNAL_MODULE) {
      md->subType = FLYSKY_SUBTYPE_AFHDS2A;
      setAvailableHandler([](int v) { return v == FLYSKY_SUBTYPE_AFHDS2A; });
    } else {
      md->subType = FLYSKY_SUBTYPE_AFHDS3;
      setAvailableHandler([](int v) { return v == FLYSKY_SUBTYPE_AFHDS3; });
    }
  }
#endif
#if defined(MULTIMODULE)
  else if (isModuleMultimodule(moduleIdx)) {
    setMin(0);
    setMax(0);
    values.clear();

    auto protos = MultiRfProtocols::instance(moduleIdx);
    protos->triggerScan();

    if (protos->isScanning()) {
      new RfScanDialog(parent, protos, [=](){ update(); });
    } else {
      TRACE("!protos->isScanning()");
    }

    setTextHandler([=](int value) { return protos->getProtoLabel(value); });

    setGetValueHandler(GET_DEFAULT(md->multi.rfProtocol));
    setSetValueHandler([=](int newValue) {
        md->multi.rfProtocol = newValue;
        md->subType = 0;
        resetMultiProtocolsOptions(moduleIdx);

        MultiModuleStatus &status = getMultiModuleStatus(moduleIdx);
        status.invalidate();

        uint32_t startUpdate = RTOS_GET_MS();
        while (!status.isValid() && (RTOS_GET_MS() - startUpdate < 250));
      });
  }
#endif
  else {
    disable();
    lv_obj_add_flag(lvobj, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  
  enable();
  lv_obj_clear_flag(lvobj, LV_OBJ_FLAG_HIDDEN);
  
  // update choice value
  lv_event_send(lvobj, LV_EVENT_VALUE_CHANGED, nullptr);
  
  // update module parameters
  
}

void ModuleSubTypeChoice::openMenu()
{
  if (isModuleMultimodule(moduleIdx)) {
    auto menu = new Menu(this);

    if (!menuTitle.empty()) menu->setTitle(menuTitle);
    menu->setCloseHandler([=]() { setEditMode(false); });

    setEditMode(true);
    invalidate();

    auto protos = MultiRfProtocols::instance(moduleIdx);
    protos->fillList([=](const MultiRfProtocols::RfProto &p) {
        addValue(p.label.c_str());
        menu->addLine(p.label.c_str(), [=]() {
            setValue(p.proto);
            lv_event_send(lvobj, LV_EVENT_VALUE_CHANGED, nullptr);
          });
      });

    ModuleData* md = &g_model.moduleData[moduleIdx];
    int idx = protos->getIndex(md->multi.rfProtocol);
    if (idx >= 0) menu->select(idx);
  } else {
    Choice::openMenu();
  }
}

static void update_module_window(lv_event_t* e)
{
  ModuleWindow* mw = (ModuleWindow*)lv_event_get_user_data(e);
  if (!mw) return;

  mw->update();
}

ModulePage::ModulePage(uint8_t moduleIdx) : Page(ICON_MODEL_SETUP)
{
  const char* title = moduleIdx == INTERNAL_MODULE ?
    STR_INTERNALRF : STR_EXTERNALRF;

  new StaticText(&header,
                 {PAGE_TITLE_LEFT, PAGE_TITLE_TOP, LCD_W - PAGE_TITLE_LEFT,
                  PAGE_LINE_HEIGHT},
                 title, 0, COLOR_THEME_PRIMARY2);

  FormGridLayout grid;
  // lv_obj_set_height(body.getLvObj(), LV_SIZE_CONTENT);
  
  // Module Type
  new StaticText(&body, grid.getLabelSlot(), STR_MODE, 0,
                 COLOR_THEME_PRIMARY1);

  ModuleData* md = &g_model.moduleData[moduleIdx];
  auto moduleChoice = new Choice(
      &body, grid.getFieldSlot(2, 0), STR_INTERNAL_MODULE_PROTOCOLS,
      MODULE_TYPE_NONE, MODULE_TYPE_COUNT - 1,
      GET_DEFAULT(md->type));

  moduleChoice->setAvailableHandler([=](int8_t moduleType) {
    return moduleIdx == INTERNAL_MODULE ? isInternalModuleAvailable(moduleType)
                                        : isExternalModuleAvailable(moduleType);
  });

  auto subTypeChoice = new ModuleSubTypeChoice(&body, grid.getFieldSlot(2, 1), moduleIdx);  
  grid.nextLine();
  
  coord_t y = grid.getWindowHeight();
  // coord_t h = body.height() - y;
  coord_t w = body.width();

  auto moduleWindow = new ModuleWindow(&body, {0, y, w, 0}, moduleIdx);
  lv_obj_set_height(moduleWindow->getLvObj(), LV_SIZE_CONTENT);

  // This needs to be after moduleWindow has been created
  moduleChoice->setSetValueHandler([=](int32_t newValue) {
    setModuleType(moduleIdx, newValue);

    subTypeChoice->update();
    moduleWindow->update();
    SET_DIRTY();
  });

  lv_obj_add_event_cb(subTypeChoice->getLvObj(), update_module_window,
                      LV_EVENT_VALUE_CHANGED, moduleWindow);
}
