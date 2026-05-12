/*
 * 文件说明: 日程服务私有实现。
 * 文件功能: 解析 schedule.csv，计算当日提醒、下一次唤醒时间，并把提醒写入中断队列。
 *
 * 函数表:
 * - ds1302DateTimeToUnix: 模块内部辅助函数。
 * - readRtcUnix: 读取、获取或消费对应数据。
 * - reminderEntryPriority: 模块内部辅助函数。
 * - isLeapYearLocal: 计算、判断或转换对应结果。
 * - maxDayInMonthLocal: 计算、判断或转换对应结果。
 * - civilFromDays: 模块内部辅助函数。
 * - unixToDs1302DateTime: 模块内部辅助函数。
 * - weekdayMondayOneFromDays: 计算、判断或转换对应结果。
 * - weekdayMondayOneFromUnix: 计算、判断或转换对应结果。
 * - weekdayMondayOneFromCivil: 计算、判断或转换对应结果。
 * - parseStrictInt: 解析、规范化或格式化对应内容。
 * - decodeScheduleMessageValue: 解析、规范化或格式化对应内容。
 * - parseStrictIntWithTail: 解析、规范化或格式化对应内容。
 * - parseScheduleCsvLine: 解析、规范化或格式化对应内容。
 * - openScheduleCsvRead: 初始化或确保对应资源可用。
 * - loadReminderSchedule: 读取、获取或消费对应数据。
 * - dateKeyFromDateTime: 模块内部辅助函数。
 * - reminderEntryMatchesToday: 模块内部辅助函数。
 * - loadReminderMessageBySourceIndex: 读取、获取或消费对应数据。
 * - loadReminderMessageByDateMinute: 读取、获取或消费对应数据。
 * - refreshTodayReminderSlots: 模块内部辅助函数。
 * - refreshTodayReminderSlotsFromRtc: 模块内部辅助函数。
 * - replaceScheduleInterruptQueueBySlots: 模块内部辅助函数。
 * - serviceScheduleInterruptByRtcMinute: 周期服务函数，维护对应后台状态。
 * - markCurrentMinuteScheduleAsTriggeredFromRtc: 模块内部辅助函数。
 * - computeNextReminderDelta: 计算、判断或转换对应结果。
 */
bool ds1302DateTimeToUnix(const Ds1302DateTime &dt, uint64_t &outUnix)
{
  if (!ds1302IsValidDateTime(dt))
    return false;
  const int64_t days = daysFromCivil(static_cast<int>(dt.year), dt.month, dt.day);
  const int64_t seconds = days * 86400LL + static_cast<int64_t>(dt.hour) * 3600LL +
                          static_cast<int64_t>(dt.minute) * 60LL + static_cast<int64_t>(dt.second);
  if (seconds < 0)
    return false;
  outUnix = static_cast<uint64_t>(seconds);
  return true;
}

bool readRtcUnix(uint64_t &outUnix)
{
  Ds1302DateTime dt;
  if (!rtc.readDateTime(dt))
    return false;
  if (!ds1302IsValidDateTime(dt))
    return false;
  return ds1302DateTimeToUnix(dt, outUnix);
}

struct ReminderSchedule
{
  struct Entry
  {
    uint16_t sourceIndex = 0;
    uint16_t number = 0;
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t week = 0; // Monday=1 ... Sunday=7
    uint16_t intervalSec = 0;
    uint16_t reminderTimes = 0;
    bool repeatDay = false;
    bool repeatMonth = false;
    bool repeatWeek = false;
    bool repeatYear = false;
    char message[kSleepTextMaxLen + 1] = {0};
  };

  Entry entries[kMaxReminderTimes];
  size_t count = 0;
};

ReminderSchedule &scheduleScratchBuffer()
{
  static ReminderSchedule schedule;
  return schedule;
}

uint8_t reminderEntryPriority(const ReminderSchedule::Entry &entry)
{
  return reminderRepeatPriority(entry.repeatDay, entry.repeatMonth, entry.repeatWeek, entry.repeatYear);
}

bool isLeapYearLocal(uint16_t year)
{
  if ((year % 4U) != 0U)
    return false;
  if ((year % 100U) != 0U)
    return true;
  return (year % 400U) == 0U;
}

uint8_t maxDayInMonthLocal(uint16_t year, uint8_t month)
{
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12)
    return 0;
  if (month == 2 && isLeapYearLocal(year))
    return 29;
  return kDays[month - 1];
}

void civilFromDays(int64_t z, int &year, unsigned &month, unsigned &day)
{
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - static_cast<int64_t>(era) * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += (month <= 2);
}

bool unixToDs1302DateTime(uint64_t unixSeconds, Ds1302DateTime &out)
{
  const uint64_t days = unixSeconds / 86400ULL;
  const uint32_t secOfDay = static_cast<uint32_t>(unixSeconds % 86400ULL);
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(static_cast<int64_t>(days), year, month, day);

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(secOfDay / 3600U);
  out.minute = static_cast<uint8_t>((secOfDay % 3600U) / 60U);
  out.second = static_cast<uint8_t>(secOfDay % 60U);
  return ds1302IsValidDateTime(out);
}

uint8_t weekdayMondayOneFromDays(int64_t daysSinceEpoch)
{
  int weekday = static_cast<int>((daysSinceEpoch + 3LL) % 7LL); // 1970-01-01 is Thursday.
  if (weekday < 0)
    weekday += 7;
  return static_cast<uint8_t>(weekday + 1); // Monday=1 ... Sunday=7
}

uint8_t weekdayMondayOneFromUnix(uint64_t unixSeconds)
{
  return weekdayMondayOneFromDays(static_cast<int64_t>(unixSeconds / 86400ULL));
}

uint8_t weekdayMondayOneFromCivil(uint16_t year, uint8_t month, uint8_t day)
{
  return weekdayMondayOneFromDays(daysFromCivil(static_cast<int>(year), month, day));
}

bool parseStrictInt(String value, int &out)
{
  value.trim();
  if (!value.length())
    return false;
  int start = 0;
  if (value[0] == '+' || value[0] == '-')
  {
    if (value.length() == 1)
      return false;
    start = 1;
  }
  for (int i = start; i < value.length(); ++i)
  {
    if (!isDigit(value[i]))
      return false;
  }
  out = value.toInt();
  return true;
}

String decodeScheduleMessageValue(String value)
{
  value.trim();
  if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
  {
    value = value.substring(1, value.length() - 1);
    value.replace("\"\"", "\"");
  }
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();
  return value;
}

bool parseStrictIntWithTail(String value, int &out, String *outTail)
{
  value.trim();
  if (outTail)
    outTail->remove(0);
  if (!value.length())
    return false;
  int idx = 0;
  if (value[idx] == '+' || value[idx] == '-')
  {
    if (value.length() == 1)
      return false;
    ++idx;
  }
  const int digitStart = idx;
  while (idx < value.length() && isDigit(value[idx]))
  {
    ++idx;
  }
  if (idx == digitStart)
    return false;
  const String numPart = value.substring(0, idx);
  if (!parseStrictInt(numPart, out))
    return false;
  if (outTail)
  {
    *outTail = value.substring(idx);
    outTail->trim();
  }
  return true;
}

bool parseScheduleCsvLine(const String &lineRaw, ReminderSchedule::Entry &outEntry, String *outMessage)
{
  String line = lineRaw;
  line.trim();
  if (!line.length())
    return false;
  if (line.startsWith("#") || line.startsWith(";"))
    return false;
  if (outMessage)
  {
    *outMessage = "";
  }

  String cols[14];
  size_t colCount = 0;
  int start = 0;
  while (start <= line.length())
  {
    if (colCount >= 13)
    {
      cols[colCount++] = line.substring(start);
      cols[colCount - 1].trim();
      break;
    }
    const int comma = line.indexOf(',', start);
    String token = (comma >= 0) ? line.substring(start, comma) : line.substring(start);
    token.trim();
    cols[colCount++] = token;
    if (comma < 0)
      break;
    start = comma + 1;
  }

  if (colCount < 11)
    return false;
  const bool isV2 = (colCount >= 13);

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int week = 0;
  int repeatDay = 0;
  int repeatMonth = 0;
  int repeatWeek = 0;
  int repeatYear = 0;
  int number = 0;
  int intervalSec = 0;
  int reminderTimes = 0;
  String messageTailFromTimes;

  if (!parseStrictInt(cols[0], year) || year < 2000 || year > 2099)
    return false;
  if (!parseStrictInt(cols[1], month) || month < 1 || month > 12)
    return false;
  if (!parseStrictInt(cols[2], day) || day < 1 || day > 31)
    return false;
  if (!parseStrictInt(cols[3], hour) || hour < 0 || hour > 23)
    return false;
  if (!parseStrictInt(cols[4], minute) || minute < 0 || minute > 59)
    return false;
  if (isV2)
  {
    if (!parseStrictInt(cols[5], week) || week < 0 || week > 7)
      return false;
    if (!parseStrictInt(cols[6], repeatDay) || (repeatDay != 0 && repeatDay != 1))
      return false;
    if (!parseStrictInt(cols[7], repeatMonth) || (repeatMonth != 0 && repeatMonth != 1))
      return false;
    if (!parseStrictInt(cols[8], repeatWeek) || (repeatWeek != 0 && repeatWeek != 1))
      return false;
    if (!parseStrictInt(cols[9], repeatYear) || (repeatYear != 0 && repeatYear != 1))
      return false;
    if (!parseStrictInt(cols[10], number) || number < 0 || number > 65535)
      return false;
    if (!parseStrictInt(cols[11], intervalSec) || intervalSec < 0 || intervalSec > 65535)
      return false;
    if (!parseStrictIntWithTail(cols[12], reminderTimes, &messageTailFromTimes) || reminderTimes < 0 ||
        reminderTimes > 65535)
    {
      return false;
    }
    second = 0;
  }
  else
  {
    if (!parseStrictInt(cols[5], second) || second < 0 || second > 59)
      return false;
    if (!parseStrictInt(cols[6], week) || week < 0 || week > 7)
      return false;
    if (!parseStrictInt(cols[7], repeatDay) || (repeatDay != 0 && repeatDay != 1))
      return false;
    if (!parseStrictInt(cols[8], repeatMonth) || (repeatMonth != 0 && repeatMonth != 1))
      return false;
    if (!parseStrictInt(cols[9], repeatWeek) || (repeatWeek != 0 && repeatWeek != 1))
      return false;
    if (!parseStrictInt(cols[10], repeatYear) || (repeatYear != 0 && repeatYear != 1))
      return false;
    number = 0;
    intervalSec = 0;
    reminderTimes = 0;
  }

  Ds1302DateTime base;
  base.year = static_cast<uint16_t>(year);
  base.month = static_cast<uint8_t>(month);
  base.day = static_cast<uint8_t>(day);
  base.hour = static_cast<uint8_t>(hour);
  base.minute = static_cast<uint8_t>(minute);
  base.second = static_cast<uint8_t>(second);
  if (!ds1302IsValidDateTime(base))
    return false;

  outEntry.year = base.year;
  outEntry.month = base.month;
  outEntry.day = base.day;
  outEntry.hour = base.hour;
  outEntry.minute = base.minute;
  outEntry.second = 0; // Minute precision: ignore second field from CSV.
  outEntry.week =
      (week == 0) ? weekdayMondayOneFromCivil(outEntry.year, outEntry.month, outEntry.day) : static_cast<uint8_t>(week);
  outEntry.repeatDay = (repeatDay != 0);
  outEntry.repeatMonth = (repeatMonth != 0);
  outEntry.repeatWeek = (repeatWeek != 0);
  outEntry.repeatYear = (repeatYear != 0);
  outEntry.number = static_cast<uint16_t>(number);
  outEntry.intervalSec = static_cast<uint16_t>(intervalSec);
  outEntry.reminderTimes = static_cast<uint16_t>(reminderTimes);

  String messageField;
  if (isV2)
  {
    if (colCount > 13)
    {
      messageField = cols[13];
    }
    else if (messageTailFromTimes.length())
    {
      messageField = messageTailFromTimes;
    }
  }
  else if (colCount > 11)
  {
    messageField = cols[11];
  }
  String decoded = decodeScheduleMessageValue(messageField);
  if (!decoded.length())
  {
    decoded = kDefaultReminderMessage;
  }
  sanitizeMessageForSnapshot(decoded, outEntry.message);
  if (outMessage)
  {
    *outMessage = decoded;
  }
  return true;
}

bool openScheduleCsvRead(fs::File &outFile, bool &outMountedTemp)
{
  outMountedTemp = false;
  if (!fatMounted)
  {
    if (!mountFat())
    {
      return false;
    }
    outMountedTemp = true;
  }

  outFile = FFat.open("/schedule.csv", FILE_READ);
  if (!outFile)
  {
    if (outMountedTemp)
    {
      unmountFat();
      outMountedTemp = false;
    }
    return false;
  }
  return true;
}

bool loadReminderSchedule(ReminderSchedule &outSchedule)
{
  outSchedule.count = 0;

  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp))
    return false;

  while (f.available() && outSchedule.count < kMaxReminderTimes)
  {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    if (parseScheduleCsvLine(line, entry, nullptr))
    {
      entry.sourceIndex = static_cast<uint16_t>(outSchedule.count);
      if (entry.number == 0)
      {
        entry.number = entry.sourceIndex;
      }
      outSchedule.entries[outSchedule.count++] = entry;
    }
  }
  f.close();

  if (mountedTemp)
  {
    unmountFat();
  }

  return outSchedule.count > 0;
}

uint32_t dateKeyFromDateTime(const Ds1302DateTime &dt)
{
  return static_cast<uint32_t>(dt.year) * 10000U + static_cast<uint32_t>(dt.month) * 100U +
         static_cast<uint32_t>(dt.day);
}

bool reminderEntryMatchesToday(const ReminderSchedule::Entry &entry, const Ds1302DateTime &today, uint8_t todayWeek)
{
  const bool anyRepeat = entry.repeatDay || entry.repeatMonth || entry.repeatWeek || entry.repeatYear;
  if (!anyRepeat)
  {
    return entry.year == today.year && entry.month == today.month && entry.day == today.day;
  }
  if (entry.repeatDay)
    return true;
  if (entry.repeatWeek && entry.week == todayWeek)
    return true;
  if (entry.repeatMonth && entry.day == today.day)
    return true;
  if (entry.repeatYear && entry.month == today.month && entry.day == today.day)
    return true;
  return false;
}

bool loadReminderMessageBySourceIndex(uint16_t sourceIndex, String &outMessage)
{
  outMessage = "";
  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp))
    return false;

  uint16_t currentIndex = 0;
  while (f.available())
  {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    String message;
    if (!parseScheduleCsvLine(line, entry, &message))
      continue;

    if (currentIndex == sourceIndex)
    {
      f.close();
      if (mountedTemp)
      {
        unmountFat();
      }
      message.trim();
      if (!message.length())
      {
        message = kDefaultReminderMessage;
      }
      outMessage = message;
      return true;
    }
    ++currentIndex;
  }

  f.close();
  if (mountedTemp)
  {
    unmountFat();
  }
  return false;
}

bool loadReminderMessageByDateMinute(const Ds1302DateTime &today, uint8_t hour, uint8_t minute, String &outMessage)
{
  outMessage = "";
  fs::File f;
  bool mountedTemp = false;
  if (!openScheduleCsvRead(f, mountedTemp))
    return false;

  const uint8_t todayWeek = weekdayMondayOneFromCivil(today.year, today.month, today.day);
  while (f.available())
  {
    const String line = f.readStringUntil('\n');
    ReminderSchedule::Entry entry;
    String message;
    if (!parseScheduleCsvLine(line, entry, &message))
      continue;
    if (!reminderEntryMatchesToday(entry, today, todayWeek))
      continue;
    if (entry.hour != hour || entry.minute != minute)
      continue;

    f.close();
    if (mountedTemp)
    {
      unmountFat();
    }
    message.trim();
    if (!message.length())
    {
      message = kDefaultReminderMessage;
    }
    outMessage = message;
    return true;
  }

  f.close();
  if (mountedTemp)
  {
    unmountFat();
  }
  return false;
}

void refreshTodayReminderSlots(const Ds1302DateTime &nowDt, bool markPastTriggered)
{
  ReminderSchedule &schedule = scheduleScratchBuffer();
  const bool loaded = loadReminderSchedule(schedule);
  gTodayReminderCount = 0;
  gTodayReminderDateKey = dateKeyFromDateTime(nowDt);
  gTodayReminderLastCheckedMinute = 0xFFFFU;

  if (!loaded)
  {
    Serial.printf("[SCHEDULE] today slot refresh: no schedule (%04u-%02u-%02u)\n",
                  static_cast<unsigned int>(nowDt.year), static_cast<unsigned int>(nowDt.month),
                  static_cast<unsigned int>(nowDt.day));
    return;
  }

  const uint8_t todayWeek = weekdayMondayOneFromCivil(nowDt.year, nowDt.month, nowDt.day);
  for (size_t i = 0; i < schedule.count && gTodayReminderCount < kMaxReminderTimes; ++i)
  {
    const ReminderSchedule::Entry &entry = schedule.entries[i];
    if (!reminderEntryMatchesToday(entry, nowDt, todayWeek))
      continue;
    DailyReminderSlot &slot = gTodayReminderSlots[gTodayReminderCount];
    slot.hour = entry.hour;
    slot.minute = entry.minute;
    slot.priority = reminderEntryPriority(entry);
    slot.number = entry.number;
    slot.scheduleIndex = static_cast<uint16_t>(i);
    slot.triggered = false;
    ++gTodayReminderCount;
  }

  std::sort(gTodayReminderSlots, gTodayReminderSlots + gTodayReminderCount,
            [](const DailyReminderSlot &a, const DailyReminderSlot &b)
            {
              const uint16_t aMin = static_cast<uint16_t>(a.hour) * 60U + static_cast<uint16_t>(a.minute);
              const uint16_t bMin = static_cast<uint16_t>(b.hour) * 60U + static_cast<uint16_t>(b.minute);
              if (aMin != bMin)
                return aMin < bMin;
              if (a.priority != b.priority)
                return a.priority < b.priority;
              if (a.number != b.number)
                return a.number < b.number;
              return a.scheduleIndex < b.scheduleIndex;
            });

  if (markPastTriggered)
  {
    const uint16_t nowMinuteOfDay = static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
    for (size_t i = 0; i < gTodayReminderCount; ++i)
    {
      const uint16_t slotMinuteOfDay = static_cast<uint16_t>(gTodayReminderSlots[i].hour) * 60U +
                                       static_cast<uint16_t>(gTodayReminderSlots[i].minute);
      if (slotMinuteOfDay < nowMinuteOfDay)
      {
        gTodayReminderSlots[i].triggered = true;
      }
    }
  }

  Serial.printf("[SCHEDULE] today slot refresh count=%u (%04u-%02u-%02u)\n",
                static_cast<unsigned int>(gTodayReminderCount), static_cast<unsigned int>(nowDt.year),
                static_cast<unsigned int>(nowDt.month), static_cast<unsigned int>(nowDt.day));
}

bool refreshTodayReminderSlotsFromRtc(bool forceReload, bool markPastTriggered)
{
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt))
  {
    return false;
  }
  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (!forceReload && gTodayReminderDateKey == dateKey)
  {
    return true;
  }
  refreshTodayReminderSlots(nowDt, markPastTriggered);
  return true;
}

void replaceScheduleInterruptQueueBySlots(const uint16_t *slotIndices, size_t slotCount)
{
  clearScheduleInterruptQueue();
  if (!slotIndices || slotCount == 0)
    return;

  ReminderSchedule &schedule = scheduleScratchBuffer();
  for (size_t i = 0; i < slotCount && gScheduleInterruptCount < kScheduleInterruptQueueMax; ++i)
  {
    const uint16_t slotIdx = slotIndices[i];
    if (slotIdx >= gTodayReminderCount)
      continue;
    const DailyReminderSlot &slot = gTodayReminderSlots[slotIdx];
    if (slot.scheduleIndex >= schedule.count)
      continue;
    const ReminderSchedule::Entry &entry = schedule.entries[slot.scheduleIndex];
    const String text = String(entry.message);
    (void)appendScheduleInterruptQueueItem(text, entry.intervalSec, entry.reminderTimes, false);
  }

  if (gScheduleInterruptCount > 0)
  {
    gScheduleInterruptPendingStart = true;
  }
}

void serviceScheduleInterruptByRtcMinute()
{
  const bool scheduleChanged = wirelessPortalConsumeScheduleReloadRequest();
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt))
  {
    return;
  }

  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (scheduleChanged || gTodayReminderDateKey != dateKey)
  {
    refreshTodayReminderSlots(nowDt, true);
  }

  if (gTodayReminderDateKey != dateKey || gTodayReminderCount == 0)
  {
    return;
  }

  const uint16_t nowMinuteOfDay = static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
  if (gTodayReminderLastCheckedMinute == nowMinuteOfDay)
  {
    return;
  }
  gTodayReminderLastCheckedMinute = nowMinuteOfDay;

  uint16_t dueSlots[kMaxReminderTimes];
  size_t dueCount = 0;
  for (size_t i = 0; i < gTodayReminderCount; ++i)
  {
    DailyReminderSlot &slot = gTodayReminderSlots[i];
    if (slot.triggered)
      continue;
    const uint16_t slotMinuteOfDay = static_cast<uint16_t>(slot.hour) * 60U + static_cast<uint16_t>(slot.minute);
    if (slotMinuteOfDay < nowMinuteOfDay)
    {
      slot.triggered = true;
      continue;
    }
    if (slot.hour == nowDt.hour && slot.minute == nowDt.minute)
    {
      slot.triggered = true;
      if (dueCount < kMaxReminderTimes)
      {
        dueSlots[dueCount++] = static_cast<uint16_t>(i);
      }
    }
  }

  if (dueCount > 0)
  {
    replaceScheduleInterruptQueueBySlots(dueSlots, dueCount);
    Serial.printf("[SCHEDULE] due now %02u:%02u queue=%u\n", static_cast<unsigned int>(nowDt.hour),
                  static_cast<unsigned int>(nowDt.minute), static_cast<unsigned int>(dueCount));
  }
}

static bool markCurrentMinuteScheduleAsTriggeredFromRtc()
{
  Ds1302DateTime nowDt;
  if (!rtc.readDateTime(nowDt) || !ds1302IsValidDateTime(nowDt))
  {
    return false;
  }

  const uint32_t dateKey = dateKeyFromDateTime(nowDt);
  if (gTodayReminderDateKey != dateKey)
  {
    refreshTodayReminderSlots(nowDt, true);
  }

  const uint16_t nowMinuteOfDay = static_cast<uint16_t>(nowDt.hour) * 60U + static_cast<uint16_t>(nowDt.minute);
  gTodayReminderLastCheckedMinute = nowMinuteOfDay;

  if (gTodayReminderDateKey != dateKey)
  {
    return false;
  }

  for (size_t i = 0; i < gTodayReminderCount; ++i)
  {
    DailyReminderSlot &slot = gTodayReminderSlots[i];
    if (slot.hour == nowDt.hour && slot.minute == nowDt.minute)
    {
      slot.triggered = true;
    }
  }
  return true;
}

bool computeNextReminderDelta(const ReminderSchedule &schedule, uint64_t nowUnix, uint32_t &outDeltaSec,
                              char outMessage[kSleepTextMaxLen + 1], uint16_t *outIntervalSec,
                              uint16_t *outReminderTimes)
{
  if (outIntervalSec)
  {
    *outIntervalSec = 0;
  }
  if (outReminderTimes)
  {
    *outReminderTimes = 0;
  }
  if (schedule.count == 0)
    return false;

  Ds1302DateTime nowDt;
  if (!unixToDs1302DateTime(nowUnix, nowDt))
    return false;
  const uint32_t nowSecOfDay = static_cast<uint32_t>(nowDt.hour) * 3600U + static_cast<uint32_t>(nowDt.minute) * 60U +
                               static_cast<uint32_t>(nowDt.second);
  const uint64_t dayStartUnix = nowUnix - static_cast<uint64_t>(nowSecOfDay);
  const uint8_t nowWeek = weekdayMondayOneFromUnix(nowUnix);
  uint32_t bestDelta = std::numeric_limits<uint32_t>::max();
  uint16_t bestScheduleIndex = 0xFFFFU;
  uint8_t bestPriority = 0xFFU;
  uint16_t bestNumber = 0xFFFFU;

  auto considerCandidate = [&](uint64_t candidateUnix, const ReminderSchedule::Entry &entry, uint16_t scheduleIndex)
  {
    uint32_t delta = 0;
    if (candidateUnix >= nowUnix)
    {
      const uint64_t diff = candidateUnix - nowUnix;
      if (diff > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
        return;
      delta = static_cast<uint32_t>(diff);
    }
    else
    {
      const uint64_t late = nowUnix - candidateUnix;
      if (late > kReminderTriggerWindowSec)
        return;
      delta = 0;
    }
    const uint8_t priority = reminderEntryPriority(entry);
    const uint16_t number = entry.number;
    if (delta < bestDelta || (delta == bestDelta &&
                              (priority < bestPriority ||
                               (priority == bestPriority &&
                                (number < bestNumber || (number == bestNumber && scheduleIndex < bestScheduleIndex))))))
    {
      bestDelta = delta;
      bestScheduleIndex = scheduleIndex;
      bestPriority = priority;
      bestNumber = number;
    }
  };

  auto makeUnix = [](int year, int month, int day, int hour, int minute, int second, uint64_t &outUnix) -> bool
  {
    if (month < 1 || month > 12)
      return false;
    const uint8_t maxDay = maxDayInMonthLocal(static_cast<uint16_t>(year), static_cast<uint8_t>(month));
    if (maxDay == 0)
      return false;
    if (day < 1)
      day = 1;
    if (day > maxDay)
      day = maxDay;
    Ds1302DateTime dt;
    dt.year = static_cast<uint16_t>(year);
    dt.month = static_cast<uint8_t>(month);
    dt.day = static_cast<uint8_t>(day);
    dt.hour = static_cast<uint8_t>(hour);
    dt.minute = static_cast<uint8_t>(minute);
    dt.second = static_cast<uint8_t>(second);
    if (!ds1302IsValidDateTime(dt))
      return false;
    return ds1302DateTimeToUnix(dt, outUnix);
  };

  for (size_t i = 0; i < schedule.count; ++i)
  {
    const ReminderSchedule::Entry &entry = schedule.entries[i];
    const uint32_t entrySecOfDay =
        static_cast<uint32_t>(entry.hour) * 3600U + static_cast<uint32_t>(entry.minute) * 60U;

    const bool anyRepeat = entry.repeatDay || entry.repeatMonth || entry.repeatWeek || entry.repeatYear;
    if (!anyRepeat)
    {
      uint64_t candidateUnix = 0;
      if (makeUnix(entry.year, entry.month, entry.day, entry.hour, entry.minute, 0, candidateUnix))
      {
        considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
      }
      continue;
    }

    if (entry.repeatDay)
    {
      uint64_t candidateUnix = dayStartUnix + static_cast<uint64_t>(entrySecOfDay);
      if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix)
      {
        candidateUnix += 86400ULL;
      }
      considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
    }

    if (entry.repeatWeek)
    {
      uint8_t targetWeek = entry.week;
      if (targetWeek < 1 || targetWeek > 7)
      {
        targetWeek = weekdayMondayOneFromCivil(entry.year, entry.month, entry.day);
      }
      int dayOffset = static_cast<int>(targetWeek) - static_cast<int>(nowWeek);
      if (dayOffset < 0)
        dayOffset += 7;
      uint64_t candidateUnix =
          dayStartUnix + static_cast<uint64_t>(dayOffset) * 86400ULL + static_cast<uint64_t>(entrySecOfDay);
      if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix)
      {
        candidateUnix += 7ULL * 86400ULL;
      }
      considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
    }

    if (entry.repeatMonth)
    {
      int targetYear = nowDt.year;
      int targetMonth = nowDt.month;
      uint64_t candidateUnix = 0;
      if (makeUnix(targetYear, targetMonth, entry.day, entry.hour, entry.minute, 0, candidateUnix))
      {
        if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix)
        {
          targetMonth += 1;
          if (targetMonth > 12)
          {
            targetMonth = 1;
            targetYear += 1;
          }
          if (makeUnix(targetYear, targetMonth, entry.day, entry.hour, entry.minute, 0, candidateUnix))
          {
            considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
          }
        }
        else
        {
          considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
        }
      }
    }

    if (entry.repeatYear)
    {
      int targetYear = nowDt.year;
      uint64_t candidateUnix = 0;
      if (makeUnix(targetYear, entry.month, entry.day, entry.hour, entry.minute, 0, candidateUnix))
      {
        if (candidateUnix + static_cast<uint64_t>(kReminderTriggerWindowSec) < nowUnix)
        {
          targetYear += 1;
          if (makeUnix(targetYear, entry.month, entry.day, entry.hour, entry.minute, 0, candidateUnix))
          {
            considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
          }
        }
        else
        {
          considerCandidate(candidateUnix, entry, static_cast<uint16_t>(i));
        }
      }
    }
  }

  if (bestDelta == std::numeric_limits<uint32_t>::max())
  {
    return false;
  }

  outDeltaSec = bestDelta;
  const ReminderSchedule::Entry *bestEntry =
      (bestScheduleIndex < schedule.count) ? &schedule.entries[bestScheduleIndex] : nullptr;
  if (bestEntry)
  {
    if (outIntervalSec)
    {
      *outIntervalSec = bestEntry->intervalSec;
    }
    if (outReminderTimes)
    {
      *outReminderTimes = bestEntry->reminderTimes;
    }
  }
  if (outMessage)
  {
    String message = kDefaultReminderMessage;
    if (bestEntry)
    {
      message = String(bestEntry->message);
      message.trim();
      if (!message.length())
      {
        message = kDefaultReminderMessage;
      }
    }
    sanitizeMessageForSnapshot(message, outMessage);
  }
  return true;
}
