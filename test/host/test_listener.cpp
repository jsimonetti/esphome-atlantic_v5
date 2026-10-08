// Host test: Listener (listener.{h,cpp}) - single-bus
// assembly + decode + the MAIN-quiet staleness gate, independent of ESPHome.
#include <cstdint>
#include <utility>
#include <vector>

#include "catalog.h"
#include "crc16.h"
#include "decoder.h"
#include "listener.h"
#include "test_harness.h"

namespace {

using atlantic_v5::DecodedValue;
using atlantic_v5::Listener;

// Collects DecodedValues via the Decoder::Sink C-function-pointer contract,
// mirroring test_decoder.cpp's Collector.
struct Collector {
  std::vector<DecodedValue> values;
};

void collect(void *ctx, const DecodedValue &v) { static_cast<Collector *>(ctx)->values.push_back(v); }

const DecodedValue *find(const Collector &c, atlantic_v5::EntityId id) {
  for (const auto &v : c.values)
    if (v.id == id)
      return &v;
  return nullptr;
}

// Builds a complete frame (header + payload, if any) with a correct trailing
// little-endian CRC-16/MODBUS, so expectations don't rely on a hand-computed
// magic number (mirrors test_relay.cpp's append_crc helper).
std::vector<uint8_t> build_frame(std::vector<uint8_t> header_and_payload) {
  header_and_payload.push_back(0);
  header_and_payload.push_back(0);
  uint16_t crc = atlantic_v5::crc16_modbus(header_and_payload.data(), header_and_payload.size() - 2);
  header_and_payload[header_and_payload.size() - 2] = static_cast<uint8_t>(crc & 0xFF);
  header_and_payload[header_and_payload.size() - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return header_and_payload;
}

void feed(Listener &l, const std::vector<uint8_t> &frame, uint32_t t_us) {
  for (uint8_t b : frame)
    l.push_byte(b, t_us);
}

}  // namespace

int main() {
  // --- A MAIN-origin payload frame (setpoint_min, header 0164 14B7 01, txn 0x64)
  // decodes and resets the MAIN-quiet staleness gate. ---
  {
    Listener l;
    Collector c;
    l.set_sink(collect, &c);

    // setpoint_min = 45.50C -> 0x11C6 big endian.
    auto frame = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    feed(l, frame, /*t_us=*/1'000'000);

    CHECK(find(c, atlantic_v5::ENT_SETPOINT_MIN) != nullptr);
    CHECK(find(c, atlantic_v5::ENT_SETPOINT_MIN)->f == 45.50f);
    CHECK(l.us_since_main(1'000'000) == 0);
    CHECK(l.us_since_main(1'060'000) == 60'000);
  }

  // --- An HMI-origin write frame (hmi_firmware_version, header 0165 0003 01, txn 0x65)
  // decodes but must never reset the MAIN-quiet gate (HMI going
  // quiet alone does not reset it - the mirror image, HMI staying chatty alone, must
  // likewise never mask MAIN going quiet). ---
  {
    Listener l;
    Collector c;
    l.set_sink(collect, &c);

    std::vector<uint8_t> header = {0x01, 0x65, 0x00, 0x03, 0x01, 17};
    std::vector<uint8_t> text = {'3', '.', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    header.insert(header.end(), text.begin(), text.end());
    auto frame = build_frame(header);

    for (uint8_t b : frame)
      l.push_byte(b, 5'000'000);

    CHECK(find(c, atlantic_v5::ENT_HMI_FIRMWARE_VERSION) != nullptr);
    CHECK(l.us_since_main(5'000'000) == 5'000'000);  // unaffected by the HMI frame
  }

  // --- A frame with a corrupted CRC is fail-safe discarded: no decode, no
  // staleness reset (single-bus framing resyncs, and a frame whose CRC failed is
  // never decoded). ---
  {
    Listener l;
    Collector c;
    l.set_sink(collect, &c);

    auto frame = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    frame[frame.size() - 1] ^= 0xFF;  // corrupt the CRC
    feed(l, frame, 1'000'000);

    CHECK(c.values.empty());
    CHECK(l.us_since_main(1'000'000) == 1'000'000);  // never set: still counting from t=0
  }

  // --- tick() applies the silence backstop so a partially-buffered frame
  // doesn't wedge push_byte()/handle_frame() out of ever running again. ---
  {
    Listener l;
    Collector c;
    l.set_sink(collect, &c);

    auto frame = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    // Feed all but the last byte, then let the silence backstop close it.
    for (size_t i = 0; i + 1 < frame.size(); i++)
      l.push_byte(frame[i], 0);
    l.tick(atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US);

    CHECK(l.assembler_stats().silence_closes == 1);
  }

  // --- set_frame_sink (behind frame capture): fired once per complete,
  // CRC-valid frame with the frame itself, independent of whether the header
  // is in Decoder's catalogue - never fired for a bad-CRC frame. ---
  {
    Listener l;
    std::vector<uint8_t> seen;
    uint32_t seen_t_us = 0;

    // set_frame_sink's ctx is opaque to Listener; route through a small pair
    // instead of a capturing lambda (the sink type is a plain function pointer).
    std::pair<std::vector<uint8_t> *, uint32_t *> ctx{&seen, &seen_t_us};
    l.set_frame_sink(
        [](void *raw_ctx, const atlantic_v5::Frame &f, uint32_t t_us) {
          auto *c = static_cast<std::pair<std::vector<uint8_t> *, uint32_t *> *>(raw_ctx);
          c->first->assign(f.raw(), f.raw() + f.raw_len());
          *c->second = t_us;
        },
        &ctx);

    auto frame = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    feed(l, frame, 2'000'000);
    CHECK(seen == frame);
    CHECK(seen_t_us == 2'000'000);

    // A bad-CRC frame never reaches the sink (fail-safe discard, 2.5.2 #4).
    seen.clear();
    auto bad = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    bad[bad.size() - 1] ^= 0xFF;
    feed(l, bad, 3'000'000);
    CHECK(seen.empty());
  }

  // --- has_main() distinguishes "never heard from MAIN" from "heard from MAIN
  // at t=0": the `connected` entity must read off until a frame has
  // actually been seen, which us_since_main() alone cannot express. ---
  {
    Listener l;
    CHECK(!l.has_main());

    // HMI-origin write frame: decodes, but is not MAIN traffic.
    std::vector<uint8_t> header = {0x01, 0x65, 0x00, 0x03, 0x01, 17};
    std::vector<uint8_t> text = {'3', '.', '1', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    header.insert(header.end(), text.begin(), text.end());
    feed(l, build_frame(header), 1'000'000);
    CHECK(!l.has_main());

    // Bad CRC is fail-safe discarded, so it must not count either.
    auto bad = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});
    bad[bad.size() - 1] ^= 0xFF;
    feed(l, bad, 2'000'000);
    CHECK(!l.has_main());

    feed(l, build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6}), 3'000'000);
    CHECK(l.has_main());
  }

  // --- A MAIN frame arriving at t_us == 0 still counts as seen; has_main()
  // must not be inferred from last_main_us_ being non-zero. ---
  {
    Listener l;
    feed(l, build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6}), 0);
    CHECK(l.has_main());
    CHECK(l.us_since_main(0) == 0);
  }

  // --- set_silence_us (the frame_silence knob) actually reaches the
  // assembler: the same partial frame and the same tick() instant close under
  // a shortened backstop and stay open under the default one. ---
  {
    const uint32_t short_silence = atlantic_v5::FrameAssembler::DEFAULT_SILENCE_US / 8;
    auto frame = build_frame({0x01, 0x64, 0x14, 0xB7, 0x01, 0x02, 0x11, 0xC6});

    Listener tuned;
    tuned.set_silence_us(short_silence);
    Listener stock;
    for (size_t i = 0; i + 1 < frame.size(); i++) {
      tuned.push_byte(frame[i], 0);
      stock.push_byte(frame[i], 0);
    }
    tuned.tick(short_silence);
    stock.tick(short_silence);

    CHECK(tuned.assembler_stats().silence_closes == 1);
    CHECK(stock.assembler_stats().silence_closes == 0);
  }

  TEST_MAIN_RETURN();
}
