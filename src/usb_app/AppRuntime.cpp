#include "usb_app/AppRuntime.h"

#include "AppMessageBus.h"

namespace
{
AppRuntime gRuntime;
}

AppRuntime &appRuntime()
{
  return gRuntime;
}

AppContext AppRuntime::makeContext(bool syntheticKeyPress)
{
  AppContext ctx;
  ctx.bus = &gAppMessageBus;
  ctx.nowMs = millis();
  ctx.syntheticKeyPress = syntheticKeyPress;
  return ctx;
}

bool AppRuntime::registerFeature(IAppFeature *feature)
{
  if (!feature || featureById(feature->id()))
    return false;
  if (_featureCount >= kMaxFeatures)
  {
    Serial.printf("[FEATURE] registry full, drop id=%u\n", static_cast<unsigned int>(feature->id()));
    return false;
  }
  _features[_featureCount++] = feature;
  return true;
}

void AppRuntime::setDefaultFeature(uint16_t featureId)
{
  _defaultFeatureId = featureId;
}

void AppRuntime::begin()
{
  if (_begun)
    return;

  AppContext ctx = makeContext();
  for (size_t i = 0; i < _featureCount; ++i)
  {
    (void)_features[i]->begin(ctx);
  }
  _begun = true;
  _active = featureById(_defaultFeatureId);
  if (_active)
  {
    _active->enter(ctx);
  }
}

void AppRuntime::enterFeatureById(uint16_t featureId)
{
  begin();
  IAppFeature *next = featureById(featureId);
  if (!next || next == _active)
    return;

  AppContext ctx = makeContext();
  if (_active)
  {
    _active->exit(ctx);
  }
  _active = next;
  _active->enter(ctx);
  publishFeatureEvent(AppMsgType::FeatureSelected, featureId);
}

void AppRuntime::requestMenu()
{
  enterFeatureById(APP_FEATURE_MENU);
}

void AppRuntime::dispatchMessage(const AppMessage &msg)
{
  begin();
  switch (msg.type)
  {
  case AppMsgType::MenuOpenRequested:
    requestMenu();
    return;
  case AppMsgType::FeatureSelected:
    if (msg.source != AppMsgSource::Feature)
    {
      selectFeature(msg);
    }
    return;
  case AppMsgType::FeatureFinished:
    enterFeatureById(APP_FEATURE_TEXT_PUSH);
    return;
  default:
    break;
  }

  AppContext ctx = makeContext();
  if (_active && _active->handleMessage(msg, ctx))
  {
    return;
  }
  Serial.printf("[APPBUS] unhandled type=%s source=%u priority=%u target=%u\n", appMessageTypeName(msg.type),
                static_cast<unsigned int>(msg.source), static_cast<unsigned int>(msg.priority),
                static_cast<unsigned int>(msg.targetFeature));
}

void AppRuntime::tick(bool syntheticKeyPress)
{
  begin();
  if (!_active)
    return;

  AppContext ctx = makeContext(syntheticKeyPress);
  _active->tick(ctx);
}

bool AppRuntime::activeCanInterrupt()
{
  begin();
  return !_active || _active->canInterrupt();
}

void AppRuntime::selectFeature(const AppMessage &msg)
{
  const uint16_t featureId = msg.targetFeature ? msg.targetFeature : msg.data.commandId;
  IAppFeature *next = featureById(featureId);
  if (!next)
    return;

  AppContext ctx = makeContext();
  (void)next->handleMessage(msg, ctx);
  enterFeatureById(featureId);
}

IAppFeature *AppRuntime::featureById(uint16_t featureId)
{
  for (size_t i = 0; i < _featureCount; ++i)
  {
    if (_features[i] && _features[i]->id() == featureId)
    {
      return _features[i];
    }
  }
  return nullptr;
}

void AppRuntime::publishFeatureEvent(AppMsgType type, uint16_t featureId)
{
  AppMessage msg;
  msg.type = type;
  msg.priority = AppMsgPriority::Normal;
  msg.source = AppMsgSource::Feature;
  msg.targetFeature = featureId;
  msg.data.commandId = featureId;
  (void)gAppMessageBus.publish(msg);
}
