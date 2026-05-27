/* UI output adapter for the UsbAppMode aggregation unit. */
class UsbAppUiDisplaySink final : public AppUiDisplaySink
{
public:
  void playText(const char *text) override
  {
    playMessageWithGlitch(text);
  }

  void showImage(const uint16_t *pixels, uint16_t width, uint16_t height, int16_t centerX, int16_t centerY) override
  {
    showWebInterruptImage(pixels, width, height, centerX, centerY);
  }
};

static UsbAppUiDisplaySink gUsbAppUiDisplaySink;

static void setupAppUiRuntime()
{
  static bool configured = false;
  if (configured)
    return;
  appUi().setDisplaySink(&gUsbAppUiDisplaySink);
  configured = true;
}
