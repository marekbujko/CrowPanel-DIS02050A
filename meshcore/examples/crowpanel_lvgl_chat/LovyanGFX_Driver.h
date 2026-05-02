#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>

class CrowPanelIoExpander {
public:
  void begin(TwoWire& wire, uint8_t addr = 0x18) {
    wire_ = &wire;
    addr_ = addr;
  }

  bool probe() const {
    if (!wire_) return false;
    wire_->beginTransmission(addr_);
    return wire_->endTransmission() == 0;
  }

  void setOutputMode(uint8_t bit) {
    updateBit(0x03, bit, false, 0xFF);
  }

  void writeOutput(uint8_t bit, bool high) {
    updateBit(0x01, bit, high, 0xFF);
  }

private:
  void updateBit(uint8_t reg, uint8_t bit, bool high, uint8_t fallback) {
    if (!wire_ || bit > 7) return;
    uint8_t value = fallback;
    readReg(reg, value);
    if (high) value |= (uint8_t)(1U << bit);
    else      value &= (uint8_t)~(1U << bit);
    writeReg(reg, value);
  }

  bool readReg(uint8_t reg, uint8_t& value) {
    if (!wire_) return false;
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom((int)addr_, 1) != 1) return false;
    value = (uint8_t)wire_->read();
    return true;
  }

  bool writeReg(uint8_t reg, uint8_t value) {
    if (!wire_) return false;
    wire_->beginTransmission(addr_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }

  TwoWire* wire_ = nullptr;
  uint8_t addr_ = 0x18;
};

class LGFX : public lgfx::LGFX_Device {
public:

  lgfx::Bus_RGB _bus_instance;
  lgfx::Panel_RGB _panel_instance;
//  lgfx::Light_PWM _light_instance;
  lgfx::Touch_GT911 _touch_instance;
  CrowPanelIoExpander _ioex;

  bool init_impl(bool use_reset, bool use_clear) override {
    _ioex.begin(Wire);
    // Some silent hardware revisions need a slightly longer/duplicated reset
    // sequence before RGB output starts, otherwise the panel stays in color test mode.
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (!_ioex.probe()) {
        delay(20);
        continue;
      }
      _ioex.setOutputMode(1);
      _ioex.setOutputMode(2);
      _ioex.setOutputMode(3);
      _ioex.setOutputMode(4);

      _ioex.writeOutput(1, true);
      _ioex.writeOutput(3, false);
      _ioex.writeOutput(4, true);

      pinMode(1, OUTPUT);
      digitalWrite(1, LOW);
      _ioex.writeOutput(2, false);
      delay(20);
      _ioex.writeOutput(2, true);
      delay(120);
      pinMode(1, INPUT);
      break;
    }

    return LGFX_Device::init_impl(use_reset, use_clear);
  }

  LGFX(void) {
    {
      auto cfg = _panel_instance.config();

      cfg.memory_width = 800;
      cfg.memory_height = 480;
      cfg.panel_width = 800;
      cfg.panel_height = 480;

      cfg.offset_x = 0;
      cfg.offset_y = 0;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _panel_instance.config_detail();

      cfg.use_psram = 1;

      _panel_instance.config_detail(cfg);
    }

    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;
      cfg.pin_d0 = GPIO_NUM_21;    // B0
      cfg.pin_d1 = GPIO_NUM_47;    // B1
      cfg.pin_d2 = GPIO_NUM_48;   // B2
      cfg.pin_d3 = GPIO_NUM_45;    // B3
      cfg.pin_d4 = GPIO_NUM_38;    // B4
      cfg.pin_d5 = GPIO_NUM_9;    // G0
      cfg.pin_d6 = GPIO_NUM_10;    // G1
      cfg.pin_d7 = GPIO_NUM_11;    // G2
      cfg.pin_d8 = GPIO_NUM_12;   // G3
      cfg.pin_d9 = GPIO_NUM_13;   // G4
      cfg.pin_d10 = GPIO_NUM_14;   // G5
      cfg.pin_d11 = GPIO_NUM_7;  // R0
      cfg.pin_d12 = GPIO_NUM_17;  // R1
      cfg.pin_d13 = GPIO_NUM_18;  // R2
      cfg.pin_d14 = GPIO_NUM_3;  // R3
      cfg.pin_d15 = GPIO_NUM_46;  // R4

      cfg.pin_henable = GPIO_NUM_42;
      cfg.pin_vsync = GPIO_NUM_41;
      cfg.pin_hsync = GPIO_NUM_40;
      cfg.pin_pclk = GPIO_NUM_39;
      cfg.freq_write = 14000000;

      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch = 8;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch = 8;
      cfg.pclk_idle_high = 1;

      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);

//    {
//      auto cfg = _light_instance.config();
//      cfg.pin_bl = GPIO_NUM_2;
//      _light_instance.config(cfg);
//    }
//    _panel_instance.light(&_light_instance);

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = 800;
      cfg.y_min = 0;
      cfg.y_max = 480;
      cfg.pin_int = -1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.i2c_port = I2C_NUM_0;
      cfg.pin_sda = GPIO_NUM_15;
      cfg.pin_scl = GPIO_NUM_16;
      cfg.pin_rst = -1;
      cfg.freq = 400000;  // GT911 supports up to 400kHz standard / 1MHz fast-mode; 800kHz is stable
      cfg.i2c_addr = 0x5D;  // 0x5D , 0x14
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }

  void sleep(void) {
    _ioex.writeOutput(1, false);
    _panel->setSleep(true);
  }

  void wakeup(void) {
    _ioex.writeOutput(1, true);
    _panel->setSleep(false);
  }
};
