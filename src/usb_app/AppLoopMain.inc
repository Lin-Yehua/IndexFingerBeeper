/* App message dispatch and processAppLoop entry point. Included by UsbAppMode.cpp. */
static void serviceAppMessageBus()
{
  if (!gAppMessageBus.begin())
  {
    return;
  }

  AppMessage msg;
  uint8_t drained = 0;
  while (drained < 8 && gAppMessageBus.poll(msg))
  {
    ++drained;
    switch (msg.type)
    {
    case AppMsgType::KeyPressed:
      inboxPushKey(msg.data.keyCode);
      break;
    case AppMsgType::TextPushReceived:
      inboxPushWebText(msg.text);
      break;
    case AppMsgType::ImmediateTextReceived:
      inboxSetImmediateText(msg.text);
      break;
    case AppMsgType::ImageReceived:
    {
      AppLoopImageEvent image;
      image.width = msg.data.image.width;
      image.height = msg.data.image.height;
      image.centerX = msg.data.image.centerX;
      image.centerY = msg.data.image.centerY;
      inboxSetImage(image);
      break;
    }
    case AppMsgType::ScheduleDue:
      inboxSetScheduleDue();
      break;
    case AppMsgType::PlayText:
      appUi().showPrompt(msg.text);
      break;
    case AppMsgType::ShowImage:
      appUi().showImage(gWebImageScratch, msg.data.image.width, msg.data.image.height, msg.data.image.centerX,
                           msg.data.image.centerY);
      break;
    default:
      appRuntime().dispatchMessage(msg);
      break;
    }
  }
}

void processAppLoop()
{
  setupAppUiRuntime();
  setupAppRuntime();
  serviceBatteryMonitor(false);
  appCheckLowBatterySleepIfNeeded(false, "app-loop");
  serviceLowBatteryWarning();

  const bool instantRefreshNoKey = wirelessPortalInstantRefreshNoKeyEnabled();

  dispatchModeEnterIfNeeded();
  gAppInterruptController.syncMode();
  ensureImageBuf();
  reloadCsvIfNeeded(instantRefreshNoKey);
  serviceScheduleInterruptByRtcMinute();
  pumpAppInputSources();
  serviceAppMessageBus();

  if (appRuntime().activeCanInterrupt() && gAppInterruptController.service())
    return;
  appRuntime().tick(gAppInterruptController.takeSyntheticKeyPress());
}
