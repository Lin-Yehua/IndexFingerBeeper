/* Runtime wiring for the UsbAppMode aggregation unit. */
static void setupAppRuntime()
{
  static bool registered = false;
  if (registered)
    return;

  AppRuntime &runtime = appRuntime();
  runtime.setDefaultFeature(APP_FEATURE_TEXT_PUSH);
  (void)runtime.registerFeature(&gTextPushFeature);
  (void)runtime.registerFeature(&gMenuFeature);
  (void)runtime.registerFeature(&gCoinFlipFeature);
  (void)runtime.registerFeature(&gMusicFeature);
  registered = true;
}

static void requestMenuFeature()
{
  AppMessage msg;
  msg.type = AppMsgType::MenuOpenRequested;
  msg.priority = AppMsgPriority::High;
  msg.source = AppMsgSource::Feature;
  msg.targetFeature = APP_FEATURE_MENU;
  (void)gAppMessageBus.publish(msg);
}

static AppLoopMode nextTextPushMode()
{
  switch (gAppLoopMode)
  {
  case APP_MODE_AP_STA:
    return APP_MODE_STA_ONLINE;
  case APP_MODE_STA_ONLINE:
    return APP_MODE_STA_ONLY;
  case APP_MODE_STA_ONLY:
  default:
    return APP_MODE_AP_STA;
  }
}

void MenuFeature::selectCurrentItem()
{
  if (_selected >= kAppMenuEntryCount)
    return;
  const AppMenuEntry &entry = kAppMenuEntries[_selected];
  publishFeatureSelection(entry.featureId, entry.commandId);
}
