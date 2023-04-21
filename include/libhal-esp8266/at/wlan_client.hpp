// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

#include <libhal-util/as_bytes.hpp>
#include <libhal-util/serial.hpp>
#include <libhal-util/serial_coroutines.hpp>
#include <libhal-util/streams.hpp>
#include <libhal-util/timeout.hpp>
#include <libhal/serial.hpp>
#include <libhal/socket.hpp>

#include "../util.hpp"

namespace hal::esp8266::at {

class socket;
/**
 * @brief wlan_client AT command driver for connecting to WiFi Access points
 *
 */
class wlan_client
{
public:
  enum class status
  {
    disconnected,
    connected,
  };

  static result<wlan_client> create(hal::serial& p_serial,
                                    std::string_view p_ssid,
                                    std::string_view p_password,
                                    timeout auto p_timeout)
  {
    static constexpr auto serial_settings = hal::serial::settings{
      .baud_rate = 115200,
      .stop = hal::serial::settings::stop_bits::one,
      .parity = hal::serial::settings::parity::none,
    };

    HAL_CHECK(p_serial.configure(serial_settings));
    HAL_CHECK(p_serial.flush());

    wlan_client client(p_serial);

    HAL_CHECK(client.reset(p_timeout));
    HAL_CHECK(client.connect(p_ssid, p_password, p_timeout));

    return client;
  }

  friend class socket;

private:
  /**
   * @param p_serial the serial port connected to the wlan_client
   * @param p_baud_rate the operating baud rate for the wlan_client
   *
   */
  wlan_client(hal::serial& p_serial)
    : m_serial{ &p_serial }
  {
  }

  [[nodiscard]] hal::status reset(timeout auto p_timeout)
  {
    // Reset the device
    HAL_CHECK(write(*m_serial, "AT+RST\r\n"));
    HAL_CHECK(wait_for_reset_complete(p_timeout));

    // Turn off echo
    HAL_CHECK(write(*m_serial, "ATE0\r\n"));
    HAL_CHECK(wait_for_ok(p_timeout));

    return hal::success();
  }

  [[nodiscard]] hal::status connect(std::string_view p_ssid,
                                    std::string_view p_password,
                                    timeout auto p_timeout)
  {
    // Configure as WiFi Station (client) mode
    HAL_CHECK(write(*m_serial, "AT+CWMODE=1\r\n"));
    HAL_CHECK(wait_for_ok(p_timeout));

    // Connect to wifi access point
    HAL_CHECK(write(*m_serial, "AT+CWJAP_CUR=\""));
    HAL_CHECK(write(*m_serial, p_ssid));
    HAL_CHECK(write(*m_serial, "\",\""));
    HAL_CHECK(write(*m_serial, p_password));
    HAL_CHECK(write(*m_serial, "\"\r\n"));
    HAL_CHECK(wait_for_ok(p_timeout));

    m_connected = true;
    return hal::success();
  }

  [[nodiscard]] hal::skip_past skip(std::string_view p_string)
  {
    return hal::skip_past(*m_serial, as_bytes(p_string));
  }

  [[nodiscard]] hal::status wait_for_ok(timeout auto p_timeout)
  {
    HAL_CHECK(try_until(skip(ok_response), p_timeout));

    return hal::success();
  }

  [[nodiscard]] hal::status wait_for_reset_complete(timeout auto p_timeout)
  {
    HAL_CHECK(try_until(skip(reset_complete), p_timeout));

    return hal::success();
  }

  serial* m_serial;
  bool m_connected = false;
};
}  // namespace hal::esp8266::at