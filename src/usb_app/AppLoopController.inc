/*
 * APP loop assembly.
 *
 * The implementation is split by architecture role while staying in the
 * UsbAppMode.cpp aggregation translation unit.
 */
#include "usb_app/AppLoopInbox.h"
#include "usb_app/AppRuntime.h"
#include "usb_app/AppUiRuntime.h"
#include "usb_app/AppLoopInbox.inc"
#include "usb_app/AppUiRuntime.inc"
#include "usb_app/AppLoopServices.inc"
#include "usb_app/AppInterruptController.inc"
#include "usb_app/TextPushFeature.inc"
#include "usb_app/AppFeatures.inc"
#include "usb_app/AppRuntime.inc"
#include "usb_app/AppLoopMain.inc"
