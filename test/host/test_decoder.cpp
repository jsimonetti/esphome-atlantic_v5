// M3 core test (ticket 06): payload codecs + Decoder header dispatch/catalog.
// Host-only: reads test/captures/synthetic_*.csv, no ESP headers.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "assembler.h"
#include "catalog.h"
#include "crc16.h"
#include "decoder.h"
#include "frame.h"
#include "types.h"
#include "test_harness.h"

#ifndef CAPTURES_DIR
#define CAPTURES_DIR "test/captures"
#endif

namespace {

std::vector<uint8_t> parse_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

atlantic_v5::Channel parse_channel(const std::string &s) {
  if (s == "hmi")
    return atlantic_v5::Channel::HMI;
  if (s == "main")
    return atlantic_v5::Channel::MAIN;
  return atlantic_v5::Channel::BUS;
}

struct Row {
  atlantic_v5::Channel channel;
  std::vector<uint8_t> bytes;
};

std::vector<Row> load_capture(const std::string &path) {
  std::vector<Row> rows;
  std::ifstream in(path);
  CHECK(in.is_open());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string ts, channel, hex;
    std::getline(ss, ts, ',');
    std::getline(ss, channel, ',');
    std::getline(ss, hex, ',');
    rows.push_back({parse_channel(channel), parse_hex(hex)});
  }
  return rows;
}

// Assembles every row of a capture (dual_bus per the row's own HMI/MAIN channel,
// or a single BUS assembler for a listener-style capture) and returns each
// delivered frame as a Frame, in delivery order. Mirrors test_assembler.cpp's
// feed_row helper; framing itself is M2's concern, not this test's.
std::vector<atlantic_v5::Frame> assemble_capture(const std::vector<Row> &rows, bool dual_bus) {
  atlantic_v5::FrameAssembler hmi(atlantic_v5::Channel::HMI, dual_bus);
  atlantic_v5::FrameAssembler main_asm(atlantic_v5::Channel::MAIN, dual_bus);
  atlantic_v5::FrameAssembler bus(atlantic_v5::Channel::BUS, dual_bus);
  std::vector<atlantic_v5::Frame> frames;

  for (const auto &row : rows) {
    atlantic_v5::FrameAssembler *a = dual_bus ? (row.channel == atlantic_v5::Channel::HMI ? &hmi : &main_asm) : &bus;
    for (uint8_t b : row.bytes) {
      if (a->push(b, /*t_us=*/0))
        frames.emplace_back(row.channel, a->frame(), a->frame_len());
    }
  }
  return frames;
}

// Collects DecodedValues via the Decoder::Sink C-function-pointer contract into
// a plain vector, for easy assertion.
struct Collector {
  std::vector<atlantic_v5::DecodedValue> values;
};

void collect(void *ctx, const atlantic_v5::DecodedValue &v) {
  static_cast<Collector *>(ctx)->values.push_back(v);
}

const atlantic_v5::DecodedValue *find(const Collector &c, atlantic_v5::EntityId id) {
  for (const auto &v : c.values)
    if (v.id == id)
      return &v;
  return nullptr;
}

// Builds a CRC-valid frame from a header key plus payload, so a header with no
// capture fixture can still be put through decode().
atlantic_v5::Frame make_frame(uint64_t key, const uint8_t *payload, uint8_t len) {
  uint8_t buf[atlantic_v5::MAX_FRAME] = {};
  for (int i = 0; i < atlantic_v5::HEADER_LEN; i++)
    buf[i] = static_cast<uint8_t>(key >> (8 * (atlantic_v5::HEADER_LEN - 1 - i)));
  uint8_t n = atlantic_v5::HEADER_LEN;
  if (len > 0) {
    buf[n++] = len;
    std::memcpy(buf + n, payload, len);
    n = static_cast<uint8_t>(n + len);
  }
  uint16_t crc = atlantic_v5::crc16_modbus(buf, n);
  buf[n] = static_cast<uint8_t>(crc & 0xFF);
  buf[n + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
  return atlantic_v5::Frame(atlantic_v5::Channel::BUS, buf, static_cast<uint8_t>(n + 2));
}

// Decodes one frame in isolation and asserts it emitted exactly the two
// expected entity ids, in order. Emitting the right values against a
// neighbouring id is the failure mode worth catching here.
void check_pair(uint64_t key, const uint8_t *payload, uint8_t len, atlantic_v5::EntityId first,
                atlantic_v5::EntityId second, Collector *out) {
  atlantic_v5::Frame f = make_frame(key, payload, len);
  CHECK(f.crc_valid());
  atlantic_v5::Decoder decoder;
  out->values.clear();
  decoder.decode(f, collect, out);
  CHECK(decoder.stats().length_mismatches == 0);
  CHECK(decoder.stats().unknown_headers == 0);
  CHECK(decoder.stats().unmapped_frames == 0);
  CHECK(out->values.size() == 2);
  CHECK(out->values[0].id == first);
  CHECK(out->values[1].id == second);
}

}  // namespace

int main() {
  // --- Codec unit tests: each of the seven codecs, exercised directly with
  // hand-derived expected values (independent of any dispatch/catalog code). ---
  {
    // temp: int16 BE, hundredths of a degree. 0x11D7 = 4567 -> 45.67 C.
    const uint8_t pos[] = {0x11, 0xD7};
    CHECK(atlantic_v5::codec::decode_temp(pos) == 45.67f);
    // Negative: -5.00 C == -500 hundredths == 0xFE0C.
    const uint8_t neg[] = {0xFE, 0x0C};
    CHECK(atlantic_v5::codec::decode_temp(neg) == -5.00f);
  }
  {
    const uint8_t b[] = {0x00, 0x01, 0x86, 0xA0};  // 100000
    CHECK(atlantic_v5::codec::decode_u32(b) == 100000u);
  }
  {
    const uint8_t b[] = {0x04, 0xB0};  // 1200
    CHECK(atlantic_v5::codec::decode_u16(b) == 1200u);
  }
  {
    const uint8_t zero[] = {0x00};
    const uint8_t one[] = {0x01};
    CHECK(atlantic_v5::codec::decode_bool(zero) == false);
    CHECK(atlantic_v5::codec::decode_bool(one) == true);
  }
  {
    // Well-formed: NUL-terminated, zero-padded.
    const uint8_t ok[] = {'2', '.', '9', 0x00, 0x00};
    char out[16];
    CHECK(atlantic_v5::codec::decode_text(ok, sizeof(ok), out, sizeof(out)));
    CHECK(std::strcmp(out, "2.9") == 0);
    // Malformed: last byte isn't 0x00 -> reject.
    const uint8_t bad[] = {'2', '.', '9', 0x00, 'x'};
    char untouched[16] = "sentinel";
    CHECK(!atlantic_v5::codec::decode_text(bad, sizeof(bad), untouched, sizeof(untouched)));
    CHECK(std::strcmp(untouched, "sentinel") == 0);
  }
  {
    // 00 <min:4000> <max:5000> -> 40.00 C / 50.00 C.
    const uint8_t ok[] = {0x00, 0x0F, 0xA0, 0x13, 0x88};
    float min_c = 0, max_c = 0;
    CHECK(atlantic_v5::codec::decode_minmax(ok, &min_c, &max_c));
    CHECK(min_c == 40.00f);
    CHECK(max_c == 50.00f);
    // byte 0 must be 0x00 -> reject.
    const uint8_t bad[] = {0x01, 0x0F, 0xA0, 0x13, 0x88};
    float untouched_min = -1, untouched_max = -1;
    CHECK(!atlantic_v5::codec::decode_minmax(bad, &untouched_min, &untouched_max));
    CHECK(untouched_min == -1 && untouched_max == -1);
  }
  {
    // secs_in_state1 > 0 -> active, count is field 3 verbatim.
    const uint8_t active[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x07};
    auto c1 = atlantic_v5::codec::decode_cycle(active);
    CHECK(c1.active == true);
    CHECK(c1.count == 7u);
    // secs_in_state1 == 0 -> inactive.
    const uint8_t inactive[] = {0x00, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    auto c2 = atlantic_v5::codec::decode_cycle(inactive);
    CHECK(c2.active == false);
    CHECK(c2.count == 3u);
  }

  // --- entity_name: independent literal expectations, spot-checking the map. ---
  CHECK(std::strcmp(atlantic_v5::entity_name(atlantic_v5::ENT_WATER_TEMPERATURE), "water_temperature") == 0);
  CHECK(std::strcmp(atlantic_v5::entity_name(atlantic_v5::ENT_CYCLE_3_ACTIVE), "cycle_3_active") == 0);
  CHECK(std::strcmp(atlantic_v5::entity_name(atlantic_v5::ENT_HMI_MODEL), "hmi_model") == 0);

  // --- Decoder dispatch, driven off the two synthetic captures already used by
  // M1/M2 (dual-bus + single-bus), so expected values are the same literals the
  // capture generator used to build the fixtures (an independent source: the
  // worked examples in plan 2.7), not recomputed by the code under test. ---
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_dual_bus_basic.csv");
    auto frames = assemble_capture(rows, /*dual_bus=*/true);

    atlantic_v5::Decoder decoder;
    Collector all;
    for (const auto &f : frames)
      decoder.decode(f, collect, &all);

    // Six-temperature broadcast (0164FEB006): 45.67 / 52.30 / 18.50 / 5.25 / 5.10 / 5.00.
    CHECK(find(all, atlantic_v5::ENT_WATER_TEMPERATURE)->f == 45.67f);
    CHECK(find(all, atlantic_v5::ENT_COMPRESSOR_OUTLET_TEMPERATURE)->f == 52.30f);
    CHECK(find(all, atlantic_v5::ENT_AIR_INLET_TEMPERATURE)->f == 18.50f);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_1_TEMPERATURE)->f == 5.25f);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_2_TEMPERATURE)->f == 5.10f);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_3_TEMPERATURE)->f == 5.00f);

    // Water min/max (0164FEBA03): 40.00 / 50.00.
    CHECK(find(all, atlantic_v5::ENT_WATER_TEMPERATURE_MIN)->f == 40.00f);
    CHECK(find(all, atlantic_v5::ENT_WATER_TEMPERATURE_MAX)->f == 50.00f);

    // input_i2/i1/heating_active bools (0164FF1403): 0x00, 0x01, 0x01.
    CHECK(find(all, atlantic_v5::ENT_INPUT_I2)->b == false);
    CHECK(find(all, atlantic_v5::ENT_INPUT_I1)->b == true);
    CHECK(find(all, atlantic_v5::ENT_HEATING_ACTIVE)->b == true);

    // Cycle 1 (0164FEE203): secs0=0 secs1=125 count=7 -> active, count 7.
    CHECK(find(all, atlantic_v5::ENT_CYCLE_1_ACTIVE)->b == true);
    CHECK(find(all, atlantic_v5::ENT_CYCLE_1_COUNT)->u == 7u);

    // Firmware version text (0164006401): "2.9".
    CHECK(std::strcmp(find(all, atlantic_v5::ENT_FIRMWARE_VERSION)->text, "2.9") == 0);

    // Payload-less frames not in the mapped catalogue, plus the HMI-origin write
    // and its MAIN ack on 0165FEF901: all four headers are in the *unmapped*
    // table (docs/protocol.md), so they are routine traffic, not anomalies.
    // The 5 payload-less READ requests for known headers (FEB006, FEBA03,
    // FF1403, FEE203, 006401) are silently skipped, not counted as unknown or
    // as a length mismatch (plan 2.2: the request side of a READ carries no
    // payload by design, that's not an anomaly).
    CHECK(decoder.stats().unmapped_frames == 4);
    CHECK(decoder.stats().unknown_headers == 0);
    CHECK(decoder.stats().last_unknown_header == 0);
    CHECK(decoder.stats().length_mismatches == 0);
  }
  {
    // --- A capture of nothing but unmapped traffic must leave the
    // unknown-frame diagnostics completely untouched (ticket 11). ---
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_unmapped_only.csv");
    auto frames = assemble_capture(rows, /*dual_bus=*/true);
    CHECK(frames.size() == rows.size());

    atlantic_v5::Decoder decoder;
    Collector all;
    for (const auto &f : frames)
      decoder.decode(f, collect, &all);

    CHECK(all.values.empty());
    CHECK(decoder.stats().unmapped_frames == frames.size());
    CHECK(decoder.stats().unknown_headers == 0);
    CHECK(decoder.stats().last_unknown_header == 0);
    CHECK(decoder.stats().last_unknown_payload_len == 0);
    CHECK(decoder.stats().length_mismatches == 0);
  }
  {
    // --- is_unmapped_header: every key in the table is recognised, and a key
    // in neither table is not. ---
    for (size_t i = 0; i < atlantic_v5::header::UNMAPPED_COUNT; i++)
      CHECK(atlantic_v5::is_unmapped_header(atlantic_v5::header::UNMAPPED[i]));
    for (size_t i = 0; i < atlantic_v5::header::MAPPED_COUNT; i++)
      CHECK(!atlantic_v5::is_unmapped_header(atlantic_v5::header::MAPPED[i]));
    CHECK(!atlantic_v5::is_unmapped_header(0x0164DEAD01ULL));
    CHECK(!atlantic_v5::is_unmapped_header(0));
  }
  {
    // --- last_unknown_header/_payload (plan 3.8's last_unknown_frame
    // diagnostic): remembers the most recent header that was in *neither*
    // table, as a raw uint64_t key plus its payload, ready for hex formatting
    // by the caller. ---
    // Header 0164DEAD01: absent from docs/protocol.md entirely.
    uint8_t frame[] = {0x01, 0x64, 0xDE, 0xAD, 0x01, 0x02, 0xBE, 0xEF, 0, 0};
    uint16_t crc = atlantic_v5::crc16_modbus(frame, sizeof(frame) - 2);
    frame[sizeof(frame) - 2] = static_cast<uint8_t>(crc & 0xFF);
    frame[sizeof(frame) - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    atlantic_v5::Frame f(atlantic_v5::Channel::BUS, frame, sizeof(frame));
    CHECK(f.crc_valid());

    atlantic_v5::Decoder decoder;
    Collector c;
    CHECK(decoder.stats().last_unknown_header == 0);
    decoder.decode(f, collect, &c);
    CHECK(c.values.empty());
    CHECK(decoder.stats().unknown_headers == 1);
    CHECK(decoder.stats().unmapped_frames == 0);
    CHECK(decoder.stats().last_unknown_header == 0x0164DEAD01ULL);
    CHECK(decoder.stats().last_unknown_payload_len == 2);
    CHECK(decoder.stats().last_unknown_payload[0] == 0xBE);
    CHECK(decoder.stats().last_unknown_payload[1] == 0xEF);
  }
  {
    // A length byte that overruns what was actually buffered must not make the
    // diagnostic read past the frame.
    uint8_t frame[] = {0x01, 0x64, 0xDE, 0xAD, 0x01, 0xFF, 0xBE, 0, 0};
    uint16_t crc = atlantic_v5::crc16_modbus(frame, sizeof(frame) - 2);
    frame[sizeof(frame) - 2] = static_cast<uint8_t>(crc & 0xFF);
    frame[sizeof(frame) - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    atlantic_v5::Frame f(atlantic_v5::Channel::BUS, frame, sizeof(frame));
    atlantic_v5::Decoder decoder;
    Collector c;
    decoder.decode(f, collect, &c);
    CHECK(decoder.stats().unknown_headers == 1);
    CHECK(decoder.stats().last_unknown_payload_len == 1);
    CHECK(decoder.stats().last_unknown_payload[0] == 0xBE);
  }
  {
    auto rows = load_capture(std::string(CAPTURES_DIR) + "/synthetic_single_bus_interleaved.csv");
    auto frames = assemble_capture(rows, /*dual_bus=*/false);

    atlantic_v5::Decoder decoder;
    Collector all;
    for (const auto &f : frames)
      decoder.decode(f, collect, &all);

    CHECK(std::strcmp(find(all, atlantic_v5::ENT_SERIAL_NUMBER)->text, "SN1234567890") == 0);
    CHECK(std::strcmp(find(all, atlantic_v5::ENT_POWER_BOARD_VERSION)->text, "1.4") == 0);
    CHECK(std::strcmp(find(all, atlantic_v5::ENT_CONTROLLER_MODEL)->text, "V5-CTRL") == 0);
    CHECK(find(all, atlantic_v5::ENT_SETPOINT)->f == 50.00f);
    CHECK(std::strcmp(find(all, atlantic_v5::ENT_HMI_VERSION)->text, "3.1") == 0);
    CHECK(std::strcmp(find(all, atlantic_v5::ENT_HMI_MODEL)->text, "HMI-STD") == 0);

    // Evaporator 1 min/max (0164FEC303): 4.50 / 6.00.
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_1_TEMPERATURE_MIN)->f == 4.50f);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_1_TEMPERATURE_MAX)->f == 6.00f);

    // Cycle 2 (0164FEE503): secs0=42 secs1=0 count=3 -> inactive, count 3.
    CHECK(find(all, atlantic_v5::ENT_CYCLE_2_ACTIVE)->b == false);
    CHECK(find(all, atlantic_v5::ENT_CYCLE_2_COUNT)->u == 3u);

    // Evaporator 2 min/max (0164FEC603) appears twice: once deliberately
    // truncated (never assembled into a frame at all, so it can't reach here)
    // and once clean after resync. Exactly one decoded pair, from the clean one.
    int evap2_min_count = 0;
    for (const auto &v : all.values)
      if (v.id == atlantic_v5::ENT_EVAPORATOR_2_TEMPERATURE_MIN)
        evap2_min_count++;
    CHECK(evap2_min_count == 1);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_2_TEMPERATURE_MIN)->f == 5.00f);
    CHECK(find(all, atlantic_v5::ENT_EVAPORATOR_2_TEMPERATURE_MAX)->f == 7.00f);

    // The deliberately corrupted 0164FEB006 frame (bad CRC, plan 2.6 "never
    // decode a frame whose CRC failed") must not emit a second water_temperature.
    int water_temp_count = 0;
    for (const auto &v : all.values)
      if (v.id == atlantic_v5::ENT_WATER_TEMPERATURE)
        water_temp_count++;
    CHECK(water_temp_count == 0);  // 0164FEB006 only appears in this capture as the corrupted frame
  }

  // --- Payload length mismatch: a well-formed CRC over a frame whose length
  // byte disagrees with the header's expected payload length must be rejected
  // (counted), never partially decoded. ---
  {
    // Header 0164006401 (firmware_version, expects 17), but only 3 payload bytes.
    uint8_t frame[] = {0x01, 0x64, 0x00, 0x64, 0x01, 0x03, '2', '.', '9', 0x00, 0x00};
    uint16_t crc = atlantic_v5::crc16_modbus(frame, sizeof(frame) - 2);
    frame[sizeof(frame) - 2] = static_cast<uint8_t>(crc & 0xFF);
    frame[sizeof(frame) - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    atlantic_v5::Frame f(atlantic_v5::Channel::BUS, frame, sizeof(frame));
    CHECK(f.crc_valid());

    atlantic_v5::Decoder decoder;
    Collector c;
    decoder.decode(f, collect, &c);
    CHECK(c.values.empty());
    CHECK(decoder.stats().length_mismatches == 1);
    CHECK(decoder.stats().unknown_headers == 0);
  }

  // --- The seven mapped headers that appear in no capture fixture (ticket 14):
  // compressor_outlet / air_inlet / evaporator_3 minmax, and cycles 3-6. Frames
  // are built here rather than replayed, so a wrong entity id in the dispatch
  // cannot hide behind a golden baseline. Payloads are hand-encoded from
  // docs/protocol.md's codec table; every value is distinct across headers so a
  // cross-wired id shows up as a wrong number, not a coincidence. ---
  {
    Collector c;

    // minmax: 00 <min:int16> <max:int16>, hundredths of a degree.
    const uint8_t compressor[] = {0x00, 0x0B, 0xB8, 0x1F, 0x40};  // 30.00 / 80.00
    check_pair(atlantic_v5::header::COMPRESSOR_OUTLET_TEMPERATURE_MINMAX, compressor, 5,
               atlantic_v5::ENT_COMPRESSOR_OUTLET_TEMPERATURE_MIN, atlantic_v5::ENT_COMPRESSOR_OUTLET_TEMPERATURE_MAX,
               &c);
    CHECK(c.values[0].f == 30.00f);
    CHECK(c.values[1].f == 80.00f);

    const uint8_t air_inlet[] = {0x00, 0xFE, 0x0C, 0x10, 0xCC};  // -5.00 / 43.00
    check_pair(atlantic_v5::header::AIR_INLET_TEMPERATURE_MINMAX, air_inlet, 5,
               atlantic_v5::ENT_AIR_INLET_TEMPERATURE_MIN, atlantic_v5::ENT_AIR_INLET_TEMPERATURE_MAX, &c);
    CHECK(c.values[0].f == -5.00f);
    CHECK(c.values[1].f == 43.00f);

    const uint8_t evap3[] = {0x00, 0x00, 0x7D, 0x03, 0xCF};  // 1.25 / 9.75
    check_pair(atlantic_v5::header::EVAPORATOR_3_TEMPERATURE_MINMAX, evap3, 5,
               atlantic_v5::ENT_EVAPORATOR_3_TEMPERATURE_MIN, atlantic_v5::ENT_EVAPORATOR_3_TEMPERATURE_MAX, &c);
    CHECK(c.values[0].f == 1.25f);
    CHECK(c.values[1].f == 9.75f);

    // cycle: secs_in_state0, secs_in_state1, count (three uint32 BE).
    // active == (secs_in_state1 > 0), so 3 and 5 are active, 4 and 6 are not.
    const uint8_t cycle3[] = {0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 101};
    check_pair(atlantic_v5::header::CYCLE_3, cycle3, 12, atlantic_v5::ENT_CYCLE_3_ACTIVE,
               atlantic_v5::ENT_CYCLE_3_COUNT, &c);
    CHECK(c.values[0].b == true);
    CHECK(c.values[1].u == 101u);

    const uint8_t cycle4[] = {0, 0, 0, 22, 0, 0, 0, 0, 0, 0, 0, 102};
    check_pair(atlantic_v5::header::CYCLE_4, cycle4, 12, atlantic_v5::ENT_CYCLE_4_ACTIVE,
               atlantic_v5::ENT_CYCLE_4_COUNT, &c);
    CHECK(c.values[0].b == false);
    CHECK(c.values[1].u == 102u);

    const uint8_t cycle5[] = {0, 0, 0, 0, 0, 0, 0, 33, 0, 0, 0, 103};
    check_pair(atlantic_v5::header::CYCLE_5, cycle5, 12, atlantic_v5::ENT_CYCLE_5_ACTIVE,
               atlantic_v5::ENT_CYCLE_5_COUNT, &c);
    CHECK(c.values[0].b == true);
    CHECK(c.values[1].u == 103u);

    const uint8_t cycle6[] = {0, 0, 0, 44, 0, 0, 0, 0, 0, 0, 0, 104};
    check_pair(atlantic_v5::header::CYCLE_6, cycle6, 12, atlantic_v5::ENT_CYCLE_6_ACTIVE,
               atlantic_v5::ENT_CYCLE_6_COUNT, &c);
    CHECK(c.values[0].b == false);
    CHECK(c.values[1].u == 104u);
  }

  // --- header::MAPPED and decode()'s case labels are two hand-maintained
  // lists with nothing tying them together (ticket 14). A payload-less frame
  // for a mapped header is silently skipped by design (plan 2.2: the request
  // side of a READ carries no payload), so any MAPPED key lacking a dispatch
  // arm falls to default: and shows up as an unknown or unmapped frame. ---
  {
    atlantic_v5::Decoder decoder;
    Collector c;
    for (size_t i = 0; i < atlantic_v5::header::MAPPED_COUNT; i++) {
      atlantic_v5::Frame f = make_frame(atlantic_v5::header::MAPPED[i], nullptr, 0);
      CHECK(f.crc_valid());
      CHECK(!f.has_payload());
      decoder.decode(f, collect, &c);
    }
    CHECK(c.values.empty());
    CHECK(decoder.stats().unknown_headers == 0);
    CHECK(decoder.stats().last_unknown_header == 0);
    CHECK(decoder.stats().unmapped_frames == 0);
    CHECK(decoder.stats().length_mismatches == 0);
  }

  TEST_MAIN_RETURN();
}
