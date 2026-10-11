#include "rx.hpp"

namespace vocatron {

extern "C" int vocatron_ring_rx(hackrf_transfer *transfer) {
  auto *sink = static_cast<IqSink *>(transfer->rx_ctx);
  if (transfer->valid_length <= 0) {
    sink->zero_xfers.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const auto n = static_cast<std::size_t>(transfer->valid_length);
  sink->usb_bytes.fetch_add(n, std::memory_order_relaxed);
  const std::size_t wrote = sink->ring->try_write(transfer->buffer, n);
  if (wrote < n) {
    sink->dropped.fetch_add(n - wrote, std::memory_order_relaxed);
  }
  return 0;
}

} // namespace vocatron
