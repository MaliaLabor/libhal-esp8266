#pragma once

#include <algorithm>
#include <cstdint>

#include <libhal/as_bytes.hpp>
#include <libhal/serial/coroutines.hpp>
#include <libhal/serial/interface.hpp>
#include <libhal/serial/util.hpp>
#include <libhal/socket/interface.hpp>
#include <libhal/streams.hpp>
#include <string_view>

#include "../util.hpp"
#include "wlan_client.hpp"

namespace hal::esp8266::at {

class tcp_socket : public hal::socket
{
public:
  static constexpr auto header = std::string_view("+IPD,");

  static result<tcp_socket> create(at::wlan_client& p_wlan_client,
                                   std::string_view p_domain,
                                   std::string_view p_port,
                                   timeout auto p_timeout)
  {
    auto& wlan_serial = *p_wlan_client.m_serial;
    auto expected_response = hal::as_bytes(ok_response);

    // Connect to web server
    HAL_CHECK(hal::write(wlan_serial, "AT+CIPSTART=\"TCP\",\""));
    HAL_CHECK(hal::write(wlan_serial, p_domain));
    HAL_CHECK(hal::write(wlan_serial, "\","));
    HAL_CHECK(hal::write(wlan_serial, p_port));
    HAL_CHECK(hal::write(wlan_serial, "\r\n"));
    HAL_CHECK(try_until(skip_past(wlan_serial, expected_response), p_timeout));

    return tcp_socket(wlan_serial);
  }

  tcp_socket(tcp_socket& p_other) = delete;
  tcp_socket& operator=(tcp_socket& p_other) = delete;

  tcp_socket(tcp_socket&& p_other)
    : m_serial(p_other.m_serial)
    , m_find_start_of_header(p_other.m_find_start_of_header)
    , m_parse_packet_length(p_other.m_parse_packet_length)
    , m_skip_colon(p_other.m_skip_colon)
  {
    p_other.m_serial = nullptr;
  }

  tcp_socket& operator=(tcp_socket&& p_other)
  {
    m_serial = p_other.m_serial;
    m_find_start_of_header = p_other.m_find_start_of_header;
    m_parse_packet_length = p_other.m_parse_packet_length;
    m_skip_colon = p_other.m_skip_colon;
    // Make sure that p_other doesn't automatically close our connection on
    // destruction
    p_other.m_serial = nullptr;
    return *this;
  }

  ~tcp_socket()
  {
    if (m_serial) {
      // Attempt to close the TCP socket and ignore any errors with
      // transmission
      (void)hal::write(*m_serial, "AT+CIPCLOSE\r\n");
    }
  }

private:
  tcp_socket(hal::serial& p_serial)
    : m_serial(&p_serial)
    , m_find_start_of_header(hal::as_bytes(header))
    , m_parse_packet_length{}
    , m_skip_colon(1)
  {
  }

  result<bool> find_header()
  {
    std::array<hal::byte, 1> byte_buffer;

    while (true) {
      if (terminated(m_skip_colon)) {
        if (m_packet_bytes_read == 0) {
          m_packet_bytes_read = m_parse_packet_length.value();
        }
        return true;
      }

      auto input = HAL_CHECK(m_serial->read(byte_buffer)).data;
      if (input.empty()) {
        return false;
      }
      // pass byte into the pipeline
      input | m_find_start_of_header | m_parse_packet_length | m_skip_colon;
    }
  }

  void reset()
  {
    m_find_start_of_header = hal::stream::find(hal::as_bytes(header));
    m_parse_packet_length = hal::stream::parse<size_t>();
    m_skip_colon = hal::stream::skip(1);
  }

  hal::result<write_t> driver_write(std::span<const hal::byte> p_data) noexcept
  {
    if (p_data.size() > maximum_transmit_packet_size) {
      return new_error(std::errc::file_too_large);
    }

    auto write_length = HAL_CHECK(integer_string::create(p_data.size()));
    HAL_CHECK(hal::write(*m_serial, "AT+CIPSENDBUF="));
    HAL_CHECK(hal::write(*m_serial, write_length.str()));
    HAL_CHECK(hal::write(*m_serial, "\r\n"));
    HAL_CHECK(hal::write(*m_serial, p_data));

    return write_t{ .data = p_data };
  }

  hal::result<read_t> driver_read(std::span<hal::byte> p_data) noexcept
  {
    // Format of a TCP packet for the ESP8266 AT commands:
    //
    //  +IDP,[0-9]+:[.*]{1460}
    //
    // Starts with a header, then length, then a ':' character, then 1 to 1460
    // bytes worth of payload data.

    auto destination = p_data;

    while (true) {
      if (!HAL_CHECK(find_header())) {
        break;
      }

      // Limit the read by either, whats left of the number of bytes in the
      // packet or the size of the buffer passed to us.
      const auto min = std::min(m_packet_bytes_read, destination.size());
      // Subspan and grab the first "min" number of bytes
      const auto data = destination.first(min);
      // Read byte out of serial port's buffer
      const auto bytes_read = HAL_CHECK(m_serial->read(data)).data;
      // Deduct the number of bytes left in the current packet
      m_packet_bytes_read -= bytes_read.size();
      // Move the destination buffer forward by the number of bytes read
      destination = destination.subspan(bytes_read.size());
      if (m_packet_bytes_read == 0) {
        reset();
      }
      if (bytes_read.empty() || destination.empty()) {
        break;
      }
    }

    auto delta = std::distance(p_data.begin(), destination.begin());

    return read_t{ .data = p_data.first(static_cast<size_t>(delta)) };
  }

  hal::serial* m_serial;
  hal::stream::find m_find_start_of_header;
  hal::stream::parse<size_t> m_parse_packet_length;
  hal::stream::skip m_skip_colon;
  size_t m_packet_bytes_read = 0;
};
}  // namespace hal::esp8266::at