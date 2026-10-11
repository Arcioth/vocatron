#pragma once

// Receive plumbing shared by every live path: the USB callback that copies
// raw I/Q into a queue, and the start/stop of the HackRF stream.
// Receive only: the one streaming call here is hackrf_start_rx.

#include "hackrf_session.hpp"
#include "ring.hpp"

#include <hackrf.h>

#include <atomic>
#include <cstdint>

namespace vocatron {

struct IqSink {
  SpscRing<std::uint8_t> *ring = nullptr;
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> zero_xfers{0};
  std::atomic<std::uint64_t> usb_bytes{0};
};

// Only the USB thread calls this. It copies into sink->ring and returns.
// `transfer->rx_ctx` must be an IqSink.
extern "C" int vocatron_ring_rx(hackrf_transfer *transfer);

// Prints the libhackrf error and returns false when rc is not success.
inline bool rx_check(const char *what, int rc) {
  if (rc == HACKRF_SUCCESS) {
    return true;
  }
  print_error(what, rc);
  return false;
}

// Starts receiving on construction's device; stops on destruction.
class RxStream {
public:
  explicit RxStream(hackrf_device *dev) : dev_(dev) {}
  ~RxStream() { stop(); }

  RxStream(const RxStream &) = delete;
  RxStream &operator=(const RxStream &) = delete;

  bool start(IqSink *sink) {
    if (!rx_check("hackrf_start_rx", hackrf_start_rx(dev_, vocatron_ring_rx, sink))) {
      return false;
    }
    running_ = true;
    return true;
  }

  void stop() {
    if (!running_) {
      return;
    }
    const int rc = hackrf_stop_rx(dev_);
    running_ = false;
    if (rc != HACKRF_SUCCESS && rc != HACKRF_ERROR_STREAMING_STOPPED &&
        rc != HACKRF_ERROR_STREAMING_EXIT_CALLED) {
      print_error("hackrf_stop_rx", rc);
    }
  }

private:
  hackrf_device *dev_ = nullptr;
  bool running_ = false;
};

} // namespace vocatron
