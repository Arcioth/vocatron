#pragma once

// Opens the first HackRF and closes it when the object dies.
// Receive paths call this. Nothing here tunes or streams.

#include <hackrf.h>

#include <cstdint>
#include <cstdio>

namespace vocatron {

inline void print_error(const char *what, int rc) {
  std::fprintf(stderr, "%s failed: %s (%d)\n", what,
               hackrf_error_name(static_cast<hackrf_error>(rc)), rc);
}

// Why an open failed, in words. The kernel's v4l "hackrf" module is the
// usual cause of a libusb "resource busy" on this machine.
inline void print_open_hint(int rc) {
  if (rc == HACKRF_ERROR_LIBUSB) {
    std::fprintf(stderr, "The HackRF is plugged in but USB could not open it.\n"
                         "If the kernel driver grabbed it, run: sudo modprobe -r hackrf\n"
                         "If the module is not loaded, NixOS is missing hardware.hackrf.enable\n"
                         "(and a fresh login so the plugdev group applies).\n");
  } else if (rc == HACKRF_ERROR_BUSY) {
    std::fprintf(stderr, "Another program already has the HackRF open. Close it and try again.\n");
  } else if (rc == HACKRF_ERROR_NOT_FOUND) {
    std::fprintf(stderr, "No HackRF on USB. On the PortaPack main menu choose HackRF,\n"
                         "then connect the USB cable.\n");
  }
}

class Library {
public:
  Library() {
    const int rc = hackrf_init();
    if (rc != HACKRF_SUCCESS) {
      print_error("hackrf_init", rc);
      return;
    }
    ready_ = true;
  }

  ~Library() {
    if (ready_) {
      hackrf_exit();
    }
  }

  Library(const Library &) = delete;
  Library &operator=(const Library &) = delete;

  bool ready() const { return ready_; }

private:
  bool ready_ = false;
};

class DeviceList {
public:
  DeviceList() : list_(hackrf_device_list()) {}

  ~DeviceList() {
    if (list_ != nullptr) {
      hackrf_device_list_free(list_);
    }
  }

  DeviceList(const DeviceList &) = delete;
  DeviceList &operator=(const DeviceList &) = delete;

  hackrf_device_list_t *get() const { return list_; }

private:
  hackrf_device_list_t *list_;
};

class Device {
public:
  Device() = default;

  ~Device() { close(); }

  Device(const Device &) = delete;
  Device &operator=(const Device &) = delete;

  void own(hackrf_device *dev) {
    close();
    dev_ = dev;
  }

  hackrf_device *get() const { return dev_; }

private:
  void close() {
    if (dev_ != nullptr) {
      hackrf_close(dev_);
      dev_ = nullptr;
    }
  }

  hackrf_device *dev_ = nullptr;
};

// Member order is the shutdown order: the device closes, then the USB
// list is freed, then libhackrf exits.
class Session {
public:
  Session() {
    if (!lib_.ready() || list_.get() == nullptr) {
      if (lib_.ready() && list_.get() == nullptr) {
        std::fprintf(stderr, "hackrf_device_list() returned null\n");
      }
      return;
    }

    const int count = list_.get()->devicecount;
    if (count < 1) {
      std::fprintf(stderr, "No HackRF found.\n");
      print_open_hint(HACKRF_ERROR_NOT_FOUND);
      return;
    }
    count_ = count;

    hackrf_device *raw = nullptr;
    const int open_rc = hackrf_device_list_open(list_.get(), 0, &raw);
    if (open_rc != HACKRF_SUCCESS || raw == nullptr) {
      print_error("hackrf_open", open_rc);
      print_open_hint(open_rc);
      return;
    }
    dev_.own(raw);
    serial_ = list_.get()->serial_numbers[0];
    ok_ = true;
  }

  bool ok() const { return ok_; }
  int count() const { return count_; }
  hackrf_device *get() const { return dev_.get(); }
  const char *serial() const { return serial_; }

private:
  Library lib_;
  DeviceList list_;
  Device dev_;
  const char *serial_ = nullptr;
  int count_ = 0;
  bool ok_ = false;
};

inline int print_identity(Session &session) {
  if (!session.ok()) {
    return 1;
  }

  std::printf("Found %d HackRF\n", session.count());
  std::printf("Serial number: %s\n", session.serial() != nullptr ? session.serial() : "(none)");

  uint8_t board = BOARD_ID_UNDETECTED;
  int rc = hackrf_board_id_read(session.get(), &board);
  if (rc != HACKRF_SUCCESS) {
    print_error("hackrf_board_id_read", rc);
    return 1;
  }
  std::printf("Board ID Number: %u (%s)\n", static_cast<unsigned>(board),
              hackrf_board_id_name(static_cast<hackrf_board_id>(board)));

  char firmware[256] = {};
  rc = hackrf_version_string_read(session.get(), firmware,
                                  static_cast<uint8_t>(sizeof(firmware) - 1));
  if (rc != HACKRF_SUCCESS) {
    print_error("hackrf_version_string_read", rc);
    return 1;
  }

  uint16_t api = 0;
  rc = hackrf_usb_api_version_read(session.get(), &api);
  if (rc != HACKRF_SUCCESS) {
    print_error("hackrf_usb_api_version_read", rc);
    return 1;
  }
  std::printf("Firmware Version: %s (API:%x.%02x)\n", firmware, (api >> 8) & 0xFFu, api & 0xFFu);

  read_partid_serialno_t part{};
  rc = hackrf_board_partid_serialno_read(session.get(), &part);
  if (rc != HACKRF_SUCCESS) {
    print_error("hackrf_board_partid_serialno_read", rc);
    return 1;
  }
  std::printf("Part ID Number: 0x%08x 0x%08x\n", static_cast<unsigned>(part.part_id[0]),
              static_cast<unsigned>(part.part_id[1]));
  return 0;
}

} // namespace vocatron
