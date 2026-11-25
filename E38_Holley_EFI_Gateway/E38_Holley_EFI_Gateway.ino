#include <Arduino.h>
#include <SPI.h>
#include <driver/twai.h>
#include <mcp_can.h>
#include <cmath>
#include <cstring>
#include "Dashboard.h"

// Raspberry Pi can0 -> CAN1 (GM), can1 -> CAN2 (Holley).
constexpr gpio_num_t Can1Rx = GPIO_NUM_6;
constexpr gpio_num_t Can1Tx = GPIO_NUM_7;
constexpr int Can2Cs = 10;
constexpr int Can2Sck = 12;
constexpr int Can2Mosi = 11;
constexpr int Can2Miso = 13;

constexpr uint32_t GmEngineStatus5 = 0x4D1;
constexpr uint32_t GmEngineStatus2 = 0x3D1;
constexpr uint32_t GmEngineStatus3 = 0x3F9;
constexpr uint32_t GmOutsideAir = 0x3F1;
constexpr uint32_t GmTransmissionStatus2 = 0x1F5;

constexpr unsigned long HolleyGear = 0x1E4FD000UL;
constexpr unsigned long HolleyPark = 0x1E52D000UL;
constexpr unsigned long HolleyMask = 0x1FFFF800UL;
constexpr unsigned long HolleyFuelLevel = 0x1E202001UL;
constexpr unsigned long HolleyAcKick = 0x1E206001UL;
constexpr unsigned long HolleyAcPressure = 0x1E20A001UL;
constexpr unsigned long HolleyReducedPower = 0x1E20E001UL;
constexpr unsigned long HolleyOutsideAir = 0x1E212001UL;
constexpr unsigned long McpExtended = 0x80000000UL;
constexpr unsigned long McpRemote = 0x40000000UL;

SPIClass can2Spi(HSPI);
MCP_CAN can2(&can2Spi, Can2Cs);

bool gatewayReady = false;
bool parkFlag = true;
uint32_t can1TxFailures = 0;
uint32_t can2TxFailures = 0;

bool setupCan2()
{
  can2Spi.begin(Can2Sck, Can2Miso, Can2Mosi, Can2Cs);
  if (can2.begin(MCP_STDEXT, CAN_1000KBPS, MCP_16MHZ) != CAN_OK)
    return false;

  if (can2.init_Mask(0, 1, HolleyMask) != CAN_OK ||
      can2.init_Mask(1, 1, HolleyMask) != CAN_OK)
    return false;

  // Duplicate both IDs across both filter banks so either RX buffer can
  // accept either input. The software check also enforces extended data frames.
  for (byte filter = 0; filter < 6; ++filter)
  {
    const unsigned long id = (filter % 2 == 0) ? HolleyGear : HolleyPark;
    if (can2.init_Filt(filter, 1, id) != CAN_OK)
      return false;
  }

  return can2.setMode(MCP_NORMAL) == CAN_OK;
}

void sendHolleyFloat(unsigned long id, float value)
{
  static_assert(sizeof(float) == sizeof(uint32_t), "Holley needs 32-bit float");
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  byte data[] = {
    static_cast<byte>(bits >> 24),
    static_cast<byte>(bits >> 16),
    static_cast<byte>(bits >> 8),
    static_cast<byte>(bits)
  };

  const byte result = can2.sendMsgBuf(id, 1, sizeof(data), data);
  if (result != CAN_OK)
    ++can2TxFailures;
  portENTER_CRITICAL(&dashboardLock);
  switch (result)
  {
    case CAN_OK: ++dashboard.can2Ok; break;
    case CAN_SENDMSGTIMEOUT: ++dashboard.can2Pending; break;
    case CAN_GETTXBFTIMEOUT: ++dashboard.can2NoBuffer; break;
    default: ++dashboard.can2Other; break;
  }
  portEXIT_CRITICAL(&dashboardLock);
  unsigned index;
  switch (id)
  {
    case HolleyFuelLevel: index = 0; break;
    case HolleyReducedPower: index = 1; break;
    case HolleyAcKick: index = 2; break;
    case HolleyAcPressure: index = 3; break;
    case HolleyOutsideAir: index = 4; break;
    default: return;
  }
  recordParameter(index, value, data, sizeof(data), result);
}

void translateGmFrame(const twai_message_t &frame)
{
  if (frame.extd || frame.rtr || frame.data_length_code > sizeof(frame.data))
    return;

  const auto &data = frame.data;
  const byte length = frame.data_length_code;
  switch (frame.identifier)
  {
    case GmEngineStatus5:
      if (length >= 6)
      {
        sendHolleyFloat(HolleyFuelLevel, data[5] * 0.392157);
        sendHolleyFloat(HolleyReducedPower, (data[3] >> 7) & 1);
      }
      break;
    case GmEngineStatus2:
      if (length >= 5)
        sendHolleyFloat(HolleyAcKick, (data[4] >> 5) & 1);
      break;
    case GmEngineStatus3:
      if (length >= 8)
        sendHolleyFloat(HolleyAcPressure, (data[7] * 14) / 6.895);
      break;
    case GmOutsideAir:
      if (length >= 8)
        sendHolleyFloat(HolleyOutsideAir, ((data[7] * 0.5 - 40) * 1.8) + 32);
      break;
  }
}

void sendGmGear(double gear)
{
  twai_message_t frame{};
  frame.identifier = GmTransmissionStatus2;
  frame.data_length_code = 8;

  if (gear == 0)
  {
    frame.data[0] = frame.data[1] = parkFlag ? 0x0F : 0x0D;
    frame.data[3] = parkFlag ? 0x01 : 0x03;
  }
  else if (gear == -1)
  {
    frame.data[0] = frame.data[1] = 0x0E;
    frame.data[3] = 0x02;
    frame.data[6] = 0x02;
  }
  else if (gear > 0 && gear <= 255)
  {
    frame.data[0] = frame.data[1] = static_cast<byte>(gear);
    frame.data[3] = 0x04;
    frame.data[6] = 0x03;
  }
  else if (gear > 255)
  {
    return;
  }
  // As in the Python helper, gear codes below -1 leave the payload zeroed.

  const esp_err_t result = twai_transmit(&frame, 0);
  if (result != ESP_OK)
    ++can1TxFailures;
  recordParameter(7, gear, frame.data, frame.data_length_code, result);
}

bool pollHolley()
{
  unsigned long rawId;
  byte length;
  // Fowler's receive overload uses the raw 4-bit DLC when copying.
  byte data[15];
  if (can2.readMsgBuf(&rawId, &length, data) != CAN_OK)
    return false;

  if ((rawId & (McpExtended | McpRemote)) != McpExtended ||
      length < 4 || length > 8)
    return true;

  const unsigned long id = rawId & HolleyMask;
  if (id != HolleyPark && id != HolleyGear)
    return true;

  const uint32_t bits = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
                        (uint32_t(data[2]) << 8) | uint32_t(data[3]);
  float value;
  memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value))
    return true;

  // Match the Python listener's rounding to two decimal places.
  const double rounded = std::nearbyint(double(value) * 100.0) / 100.0;
  recordParameter(id == HolleyPark ? 5 : 6, value, data, length, 0);
  if (id == HolleyPark)
    parkFlag = rounded != 0;
  else
    sendGmGear(std::trunc(rounded));

  return true;
}

void setup()
{
  Serial.begin(115200);
  // HTTP runs separately so browser connections do not block CAN polling.
  if (xTaskCreate(dashboardTask, "dashboard", 8192, nullptr, 1, nullptr) != pdPASS)
    Serial.println("Dashboard task creation failed");
  if (!setupCan2())
  {
    Serial.println("CAN2 MCP2515 initialization failed");
    return;
  }

  twai_general_config_t config =
      TWAI_GENERAL_CONFIG_DEFAULT(Can1Tx, Can1Rx, TWAI_MODE_NORMAL);
  config.rx_queue_len = 64;
  config.tx_queue_len = 16;
  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  const esp_err_t installed = twai_driver_install(&config, &timing, &filter);
  if (installed != ESP_OK)
  {
    Serial.printf("CAN1 install failed: %s\n", esp_err_to_name(installed));
    return;
  }

  const esp_err_t started = twai_start();
  if (started != ESP_OK)
  {
    twai_driver_uninstall();
    Serial.printf("CAN1 start failed: %s\n", esp_err_to_name(started));
    return;
  }

  gatewayReady = true;
  portENTER_CRITICAL(&dashboardLock);
  dashboard.ready = true;
  portEXIT_CRITICAL(&dashboardLock);
  Serial.println("Gateway ready: CAN1 GM 500 kbps, CAN2 Holley 1 Mbps");
}

void loop()
{
  if (!gatewayReady)
  {
    delay(1000);
    return;
  }

  // Service both directions in one task. All MCP2515 SPI calls share this owner.
  bool didWork = pollHolley();
  twai_message_t gmFrame;
  if (twai_receive(&gmFrame, 0) == ESP_OK)
  {
    translateGmFrame(gmFrame);
    didWork = true;
  }

  static uint32_t lastYield = millis();
  static uint32_t lastReport = lastYield;
  const uint32_t now = millis();
  if (now - lastReport >= 1000)
  {
    lastReport = now;
    twai_status_info_t status{};
    twai_get_status_info(&status);
    const byte tec = can2.errorCountTX();
    const byte errors = can2.getError();
    portENTER_CRITICAL(&dashboardLock);
    dashboard.can1Failures = can1TxFailures;
    dashboard.can1Missed = status.rx_missed_count;
    dashboard.tec = tec;
    dashboard.errors = errors;
    portEXIT_CRITICAL(&dashboardLock);
    static uint32_t lastCan1Failures = 0;
    static uint32_t lastCan2Failures = 0;
    static uint32_t lastCan1Missed = 0;
    if (can1TxFailures != lastCan1Failures || can2TxFailures != lastCan2Failures ||
        status.rx_missed_count != lastCan1Missed)
    {
      Serial.printf("TX failures CAN1=%lu CAN2=%lu, CAN1 RX missed=%lu\n",
                    (unsigned long)can1TxFailures, (unsigned long)can2TxFailures,
                    (unsigned long)status.rx_missed_count);
      lastCan1Failures = can1TxFailures;
      lastCan2Failures = can2TxFailures;
      lastCan1Missed = status.rx_missed_count;
    }
  }

  if (!didWork || now - lastYield >= 10)
  {
    lastYield = now;
    vTaskDelay(1);
  }
}
