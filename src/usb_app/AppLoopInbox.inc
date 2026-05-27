/* Interrupt state shared by the app-loop aggregation unit. */
struct LoopIrqState
{
  bool webActive = false;
  bool webKeyLatch = false;
  bool imageActive = false;
  bool imageKeyLatch = false;
  bool imagePreemptedWeb = false;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
  int16_t imageCenterX = 160;
  int16_t imageCenterY = 155;
  bool immediateActive = false;
  bool immediateKeyLatch = false;
  bool immediatePreemptedWeb = false;
};

static void requestMenuFeature();
static AppLoopMode nextTextPushMode();
