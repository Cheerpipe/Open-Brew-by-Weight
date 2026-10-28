#define OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST
#include "../OpenBrewByWeightShotTracePool.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace openbrewbyweight;

namespace {

int failures = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "          \
                << #expression << '\n';                                       \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

using Flash = ShotTracePoolHostFlash;
constexpr size_t S = SHOT_TRACE_POOL_SECTOR_BYTES;
constexpr size_t RESERVED = SHOT_TRACE_POOL_RESERVED_SECTORS;
// Physical host sectors of shotcurve's legacy epoch sectors.
constexpr size_t LEGACY0 = RESERVED, LEGACY1 = RESERVED + 1;

ShotTracePoolIndex indexA, indexB;
uint8_t traceBuffer[16384], curveBuffer[6000];

// Deterministic sizes and content per shot id: small (padded to the minimum),
// medium, spanning several sectors, and the largest record.
struct Sizes {
  uint16_t trace, curve;
};
Sizes sizesOf(uint32_t id) {
  static const Sizes table[] = {{792, 0},     {900, 1500}, {4000, 2500}, {9001, 3001},
                                {16384, 5596}, {2047, 333}, {12000, 0},  {3333, 4444}};
  return table[id % (sizeof(table) / sizeof(table[0]))];
}
uint8_t contentByte(uint32_t id, uint8_t tag, size_t i) {
  return static_cast<uint8_t>(id * 37U + i * 13U + tag * 101U + (i >> 8));
}
ShotTracePoolIdentity identityOf(uint32_t id) {
  return {id, 1000U + id % 7U, id * 1000U + 17U, 0x01020300U, static_cast<uint8_t>(id % 5U)};
}

bool appendShot(ShotTracePool &pool, uint32_t id) {
  const Sizes sizes = sizesOf(id);
  for (size_t i = 0; i < sizes.trace; ++i) traceBuffer[i] = contentByte(id, 2, i);
  for (size_t i = 0; i < sizes.curve; ++i) curveBuffer[i] = contentByte(id, 1, i);
  ShotTracePoolSource sources[2] = {
      {ShotTracePoolSection::CURVE, 0, sizes.curve, curveBuffer, nullptr, nullptr},
      {ShotTracePoolSection::TRACE, 0, sizes.trace, traceBuffer, nullptr, nullptr}};
  return pool.append(identityOf(id), sources, 2);
}

// Opens and reads back both sections in odd chunk sizes.
ShotTracePoolStatus readShot(ShotTracePool &pool, uint32_t id, bool &matches) {
  const ShotTracePoolIdentity identity = identityOf(id);
  ShotTracePoolRef ref;
  uint8_t buffer[333];
  matches = false;
  ShotTracePoolStatus status =
      pool.open(id, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer));
  if (status != ShotTracePoolStatus::OK) return status;
  const Sizes sizes = sizesOf(id);
  matches = ref.header.shotId == id && ref.header.endReason == identity.endReason &&
            ref.header.firmwarePacked == identity.firmwarePacked;
  const struct {
    ShotTracePoolSection tag;
    size_t bytes;
  } sections[] = {{ShotTracePoolSection::CURVE, sizes.curve}, {ShotTracePoolSection::TRACE, sizes.trace}};
  for (const auto &section : sections) {
    size_t offset = 0, bytes = 0;
    status = pool.findSection(ref, section.tag, offset, bytes);
    if (status != ShotTracePoolStatus::OK) return status;
    matches = matches && bytes == section.bytes;
    for (size_t at = 0; at < bytes;) {
      const size_t piece = bytes - at < 777 ? bytes - at : 777;
      status = pool.read(ref, offset + at, buffer, piece < sizeof(buffer) ? piece : sizeof(buffer));
      if (status != ShotTracePoolStatus::OK) return status;
      for (size_t i = 0; i < (piece < sizeof(buffer) ? piece : sizeof(buffer)); ++i)
        matches = matches && buffer[i] == contentByte(id, static_cast<uint8_t>(section.tag), at + i);
      at += piece < sizeof(buffer) ? piece : sizeof(buffer);
    }
  }
  return ShotTracePoolStatus::OK;
}

bool readable(ShotTracePool &pool, uint32_t id) {
  bool matches = false;
  return readShot(pool, id, matches) == ShotTracePoolStatus::OK && matches;
}

std::vector<ShotTracePoolEntry> entries(const ShotTracePool &pool) {
  std::vector<ShotTracePoolEntry> out(pool.count());
  for (size_t n = 0; n < out.size(); ++n) CHECK(pool.entryAt(n, out[n]));
  return out;
}

bool sameEntries(const std::vector<ShotTracePoolEntry> &a, const std::vector<ShotTracePoolEntry> &b) {
  if (a.size() != b.size()) return false;
  for (size_t n = 0; n < a.size(); ++n)
    if (a[n].idTag != b[n].idTag || a[n].place != b[n].place) return false;
  return true;
}

// Last sector a record occupies, without wrapping (a wrap reads past `end`).
size_t endSector(uint32_t address, size_t bytes) {
  size_t sector = address / S, offset = address % S;
  while (bytes > S - offset) {
    bytes -= S - offset;
    ++sector;
    offset = sizeof(ShotTracePoolSectorHeader);
  }
  return sector;
}
size_t newestEndSector(const ShotTracePool &pool) {
  ShotTracePoolEntry e;
  return pool.entryAt(pool.count() - 1U, e) ? endSector(e.address(), e.bytes()) : 0;
}
// Some record starts in `reserved` and continues in shotcurve.
bool crossesPartitions(const ShotTracePool &pool) {
  for (const ShotTracePoolEntry &e : entries(pool))
    if (e.address() / S < RESERVED && endSector(e.address(), e.bytes()) >= RESERVED) return true;
  return false;
}
// Fills the ring without wrapping it until the newest record ends at or
// after `sector` (a maximal record spans 7 sectors).
uint32_t fillUntil(ShotTracePool &pool, uint32_t id, size_t sector) {
  while (pool.count() == 0 || newestEndSector(pool) < sector) CHECK(appendShot(pool, id++));
  return id;
}

// Physical host sector of a logical one.
size_t physicalOf(size_t logical) { return logical < RESERVED ? logical : logical + 2U; }

void freshFlash() {
  Flash::reset();
  // Legacy epoch sectors carry a recognisable pattern the pool must keep.
  memset(Flash::sector(LEGACY0), 0x5A, S);
  memset(Flash::sector(LEGACY1), 0xA5, S);
}

void tsp01_geometry_and_index_budget() {
  CHECK(SHOT_TRACE_POOL_RESERVED_SECTORS == 335);
  CHECK(SHOT_TRACE_POOL_SECTORS == 1337);
  CHECK(SHOT_TRACE_POOL_INDEX_CAPACITY == 2048);
  CHECK(sizeof(ShotTracePoolIndex) == 2048U * 8U + 168U);
  CHECK(shotTracePoolIdTag(5, 1, 2) != shotTracePoolIdTag(5, 1, 3));
  CHECK((shotTracePoolIdTag(5, 1, 2) & 0xFFFFFFU) == 5U);
}

void tsp02_round_trip_spanning_sectors() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(!pool.beginAppend(identityOf(1), nullptr, 0));  // Not recovered yet.
  CHECK(pool.recover());
  CHECK(pool.count() == 0);
  for (uint32_t id = 1; id <= 16; ++id) CHECK(appendShot(pool, id));
  CHECK(pool.count() == 16);
  for (uint32_t id = 1; id <= 16; ++id) CHECK(readable(pool, id));
  ShotTracePoolEntry first, maxRecord;
  CHECK(pool.entryAt(0, first));
  CHECK(first.address() == sizeof(ShotTracePoolSectorHeader));
  CHECK(pool.entryAt(7, first));
  CHECK(first.bytes() == 832);  // id 8: 32 + 4 + 0 + 4 + 792, a multiple of 16.
  CHECK(pool.entryAt(3, maxRecord));
  CHECK(maxRecord.bytes() >= 22000);  // id 4: the 16 KiB trace and a curve.
  // A missing section and an empty record.
  const ShotTracePoolIdentity identity = identityOf(8);
  ShotTracePoolRef ref;
  uint8_t buffer[64];
  CHECK(pool.open(8, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::OK);
  CHECK(ref.bytes == 832);
  size_t offset = 0, bytes = 0;
  CHECK(pool.findSection(ref, ShotTracePoolSection::CURVE, offset, bytes) == ShotTracePoolStatus::OK &&
        bytes == 0);
  CHECK(pool.read(ref, ref.bytes - 4U, buffer, 8) == ShotTracePoolStatus::IO_ERROR);
  CHECK(pool.append({77, 1, 2, 3, 0}, nullptr, 0));
  CHECK(pool.open(77, 1, 2, ref, buffer, sizeof(buffer)) == ShotTracePoolStatus::OK);
  CHECK(ref.bytes == sizeof(ShotTracePoolRecordHeader));
  CHECK(pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) ==
        ShotTracePoolStatus::NOT_FOUND);
  // An odd section length is padded to 16 bytes: 32 + 4 + 8 (5 bytes) -> 48.
  const ShotTracePoolSource odd = {ShotTracePoolSection::TRACE, 0, 5, traceBuffer, nullptr, nullptr};
  CHECK(pool.append({78, 1, 2, 3, 0}, &odd, 1));
  CHECK(pool.open(78, 1, 2, ref, buffer, sizeof(buffer)) == ShotTracePoolStatus::OK && ref.bytes == 48);
  CHECK(pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) == ShotTracePoolStatus::OK &&
        bytes == 5);
  // Nothing outside `reserved` was touched.
  for (size_t p = RESERVED; p < Flash::kSectors; ++p) CHECK(Flash::touched(p) == 0);
}

void tsp03_boot_scan_rebuilds_the_identical_index() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 9; ++id) CHECK(appendShot(pool, id));
  const std::vector<ShotTracePoolEntry> before = entries(pool);
  ShotTracePool rebooted(indexB, RESERVED);
  CHECK(rebooted.recover());
  CHECK(sameEntries(before, entries(rebooted)));
  // Appends continue right after the newest record: same placement as a
  // pool that never rebooted.
  CHECK(appendShot(pool, 10));
  CHECK(appendShot(rebooted, 10));
  ShotTracePoolEntry a, b;
  CHECK(pool.entryAt(9, a) && rebooted.entryAt(9, b) && a.place == b.place);
  CHECK(readable(rebooted, 10) && readable(rebooted, 1));
  // A third boot sees both the same.
  CHECK(pool.recover());
  CHECK(sameEntries(entries(pool), entries(rebooted)));
}

void tsp04_wrap_evicts_oldest_first() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  uint32_t id = 1;
  size_t peak = 0;
  for (; id <= 400; ++id) {
    CHECK(appendShot(pool, id));
    if (pool.count() > peak) peak = pool.count();
  }
  ShotTracePoolEntry oldest;
  CHECK(pool.entryAt(0, oldest));
  // FIFO: the survivors are exactly the newest ids, all readable.
  const size_t live = pool.count();
  CHECK(live < 400 && live + 5 < peak + 5);
  for (uint32_t n = 1; n <= 400; ++n) {
    const bool expected = n > 400U - live;
    CHECK(readable(pool, n) == expected);
  }
  ShotTracePool rebooted(indexB, RESERVED);
  CHECK(rebooted.recover());
  CHECK(sameEntries(entries(pool), entries(rebooted)));
  // Sequential wear: one erase per sector the ring advanced, every sector
  // erased equally often (give or take the lap in progress).
  uint32_t low = UINT32_MAX, high = 0, total = 0;
  for (size_t p = 0; p < RESERVED; ++p) {
    low = Flash::erases(p) < low ? Flash::erases(p) : low;
    high = Flash::erases(p) > high ? Flash::erases(p) : high;
    total += Flash::erases(p);
  }
  CHECK(high - low <= 1 && low >= 1);
  CHECK(total <= RESERVED * high);
  for (size_t p = RESERVED; p < Flash::kSectors; ++p) CHECK(Flash::touched(p) == 0);
  CHECK(Flash::sector(LEGACY0)[100] == 0x5A && Flash::sector(LEGACY1)[4000] == 0xA5);
}

void tsp05_shotcurve_opens_and_the_ring_crosses_partitions() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  uint32_t id = fillUntil(pool, 1, RESERVED - 8U);
  for (size_t p = RESERVED; p < Flash::kSectors; ++p) CHECK(Flash::touched(p) == 0);
  // The migration opens shotcurve; a recover() follows.
  CHECK(!pool.setLogicalEnd(RESERVED - 1U));
  CHECK(pool.setLogicalEnd(SHOT_TRACE_POOL_SECTORS));
  CHECK(!pool.ready());
  CHECK(pool.recover());
  const uint32_t crossing = id;
  id = fillUntil(pool, id, RESERVED + 2U);
  CHECK(Flash::touched(RESERVED + 2U) != 0);  // shotcurve sector 2 is logical 335.
  for (uint32_t n = 1; n < id; ++n) CHECK(readable(pool, n));
  CHECK(crossesPartitions(pool));
  ShotTracePool rebooted(indexB, RESERVED);
  CHECK(rebooted.setLogicalEnd(SHOT_TRACE_POOL_SECTORS) && rebooted.recover());
  CHECK(sameEntries(entries(pool), entries(rebooted)));
  CHECK(readable(rebooted, crossing) && readable(rebooted, id - 1U));
  // Fill the whole space and wrap from the last logical sector to sector 0.
  for (int n = 0; n < 700; ++n) CHECK(appendShot(pool, id++));
  CHECK(Flash::erases(physicalOf(SHOT_TRACE_POOL_SECTORS - 1U)) == 1);
  const size_t live = pool.count();
  CHECK(pool.recover() && pool.count() == live);
  for (uint32_t n = 1; n < id; ++n) CHECK(readable(pool, n) == (n >= id - live));
  CHECK(Flash::touched(LEGACY0) == 0 && Flash::touched(LEGACY1) == 0);
  for (size_t i = 0; i < S; ++i) {
    CHECK(Flash::sector(LEGACY0)[i] == 0x5A);
    CHECK(Flash::sector(LEGACY1)[i] == 0xA5);
  }
}

// Power loss at every flash operation of a scenario, for several tear
// widths, then recovery. Expectations: a record committed (or prefilled) is
// readable unless its start sector was erased during the run; whatever is
// readable has exactly its content; the index holds only known records; and
// appends continue.
struct Expected {
  std::map<uint32_t, std::vector<size_t>> sectors;  // id -> logical sectors it spans
  std::map<uint32_t, ShotTracePoolEntry> entry;      // id -> its index entry
};

void startSectors(const ShotTracePool &pool, const std::vector<uint32_t> &ids, Expected &expected) {
  for (const ShotTracePoolEntry &e : entries(pool))
    for (uint32_t id : ids) {
      const ShotTracePoolIdentity identity = identityOf(id);
      if (!shotTracePoolMatches(e, id, identity.bootId, identity.endedAtMs)) continue;
      expected.entry[id] = e;
      std::vector<size_t> &list = expected.sectors[id];
      list.clear();
      for (size_t s = e.address() / S; s <= endSector(e.address(), e.bytes()); ++s)
        list.push_back(s % pool.logicalEnd());
    }
}

// Status of a record after recovery. Records in sectors the run never
// erased must keep their exact index entry (their bytes did not change);
// the others, and every scenario record, are opened and CRC-checked.
ShotTracePoolStatus statusAfter(ShotTracePool &pool, uint32_t id, const ShotTracePoolEntry &want,
                                const std::vector<ShotTracePoolEntry> &index, bool openIt) {
  bool present = false;
  for (const ShotTracePoolEntry &e : index)
    present = present || (e.idTag == want.idTag && e.place == want.place);
  if (!openIt) return present ? ShotTracePoolStatus::OK : ShotTracePoolStatus::NOT_FOUND;
  bool matches = false;
  const ShotTracePoolStatus status = readShot(pool, id, matches);
  if (status == ShotTracePoolStatus::OK) CHECK(matches && present);
  return status;
}

void sweep(size_t end, const std::vector<uint32_t> &prefill, const std::vector<uint32_t> &scenario,
           bool clearFirst, const char *name) {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.setLogicalEnd(end) && pool.recover());
  for (uint32_t id : prefill) CHECK(appendShot(pool, id));
  std::vector<uint8_t> image(Flash::image(), Flash::image() + Flash::imageBytes());
  // Restores the prefilled image, copying back only the sectors touched since.
  std::vector<uint32_t> touchedAtImage(Flash::kSectors);
  for (size_t p = 0; p < Flash::kSectors; ++p) touchedAtImage[p] = Flash::touched(p);
  const auto restore = [&] {
    for (size_t p = 0; p < Flash::kSectors; ++p) {
      if (Flash::touched(p) == touchedAtImage[p]) continue;
      memcpy(Flash::sector(p), image.data() + p * S, S);
      touchedAtImage[p] = Flash::touched(p);
    }
  };
  // Clean run: placement of every record and the operation count.
  Expected expected;
  startSectors(pool, prefill, expected);
  const size_t opsBefore = Flash::operations();
  if (clearFirst) CHECK(pool.clear());
  for (uint32_t id : scenario) CHECK(appendShot(pool, id));
  const size_t total = Flash::operations() - opsBefore;
  startSectors(pool, scenario, expected);
  CHECK(end == RESERVED || crossesPartitions(pool));
  CHECK(expected.sectors.size() == prefill.size() + scenario.size());
  const size_t tears[] = {0, 1, 3, 7, 22, 26, 1000, 4090, Flash::kRandomTear};
  size_t runs = 0;
  for (size_t f = 0; f <= total; ++f) {
    for (size_t torn : tears) {
      restore();
      Flash::setFailAfter(-1);
      CHECK(pool.recover());
      uint32_t erasedBefore[Flash::kSectors];
      for (size_t p = 0; p < Flash::kSectors; ++p) erasedBefore[p] = Flash::erases(p);
      Flash::setFailAfter(static_cast<int>(f), torn, false, static_cast<uint32_t>(f * 7919U + 17U));
      bool cleared = false;
      if (clearFirst) cleared = pool.clear();
      std::set<uint32_t> committed;
      for (uint32_t id : scenario)
        if (appendShot(pool, id)) committed.insert(id);
      Flash::setFailAfter(-1);
      std::set<size_t> erased;  // logical sectors erased in the run, even partly
      for (size_t s = 0; s < end; ++s)
        if (Flash::erases(physicalOf(s)) != erasedBefore[physicalOf(s)]) erased.insert(s);
      CHECK(pool.recover());
      // Only known records, each once.
      std::set<uint32_t> addresses;
      const std::vector<ShotTracePoolEntry> index = entries(pool);
      for (const ShotTracePoolEntry &e : index) {
        CHECK(addresses.insert(e.address()).second);
        bool known = false;
        for (const auto &item : expected.entry) known = known || item.second.idTag == e.idTag;
        CHECK(known);
      }
      std::map<uint32_t, ShotTracePoolStatus> statuses;
      std::map<uint32_t, bool> lost, isScenario;
      for (const auto &item : expected.sectors) {
        const uint32_t id = item.first;
        for (size_t s : item.second) lost[id] = lost[id] || erased.count(s) != 0;
        isScenario[id] = std::find(scenario.begin(), scenario.end(), id) != scenario.end();
        statuses[id] = statusAfter(pool, id, expected.entry[id], index, lost[id] || isScenario[id]);
      }
      // A clear that reported failure may still have landed: then every
      // older record is hidden, otherwise none beyond the erased sectors.
      bool clearLanded = cleared;
      for (const auto &item : statuses)
        if (clearFirst && !isScenario[item.first] && !lost[item.first] &&
            item.second != ShotTracePoolStatus::OK) clearLanded = true;
      for (const auto &item : statuses) {
        const uint32_t id = item.first;
        const ShotTracePoolStatus status = item.second;
        if (status == ShotTracePoolStatus::CORRUPT) CHECK(lost[id]);
        else if (status != ShotTracePoolStatus::OK) CHECK(status == ShotTracePoolStatus::NOT_FOUND);
        const bool mustExist = isScenario[id] ? committed.count(id) != 0 : !lost[id] && !clearLanded;
        if (mustExist && status != ShotTracePoolStatus::OK) {
          std::cerr << name << " f=" << f << " torn=" << torn << " lost id " << id << '\n';
          CHECK(false);
        }
        if (!isScenario[id] && clearLanded) CHECK(status == ShotTracePoolStatus::NOT_FOUND);
      }
      // Appends continue and survive the next boot (a padded minimum record
      // and one that spans sectors).
      CHECK(appendShot(pool, 904) && appendShot(pool, 905));
      CHECK(readable(pool, 904) && readable(pool, 905));
      CHECK(pool.recover());
      CHECK(readable(pool, 904) && readable(pool, 905));
      ++runs;
    }
  }
  CHECK(runs == (total + 1U) * (sizeof(tears) / sizeof(tears[0])));
  std::cout << name << ": " << total << " operations, " << runs << " power-loss runs\n";
}

std::vector<uint32_t> ids(uint32_t from, uint32_t to) {
  std::vector<uint32_t> out;
  for (uint32_t id = from; id <= to; ++id) out.push_back(id);
  return out;
}

void tsp06_power_loss_at_every_operation_with_wrap() {
  // ~1.45 MB of prefill wraps the 1.36 MB ring: the scenario evicts.
  sweep(RESERVED, ids(1, 115), {200, 201, 203, 204, 206}, false, "TSP06");
}

void tsp07_power_loss_across_the_partition_boundary() {
  // 333 sectors of prefill, then records that cross into shotcurve.
  freshFlash();
  ShotTracePool probe(indexA, RESERVED);
  CHECK(probe.recover());
  const uint32_t id = fillUntil(probe, 1, RESERVED - 8U);
  sweep(SHOT_TRACE_POOL_SECTORS, ids(1, id - 1U), {300, 302, 303, 304}, false, "TSP07");
  CHECK(Flash::touched(LEGACY0) == 0 && Flash::touched(LEGACY1) == 0);
}

void tsp08_power_loss_during_clear() {
  sweep(RESERVED, ids(1, 20), {400, 401}, true, "TSP08");
}

// Corrupts the header CRC of logical sector `s` (a hole for recovery).
void breakSectorHeader(size_t s) { Flash::sector(physicalOf(s))[offsetof(ShotTracePoolSectorHeader, crc)] ^= 0x01; }

void tsp09_corrupt_sector_headers_are_holes() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 40; ++id) CHECK(appendShot(pool, id));
  Expected expected;
  startSectors(pool, ids(1, 40), expected);
  // A hole in the middle: records starting in it or spanning into it are
  // lost; the walk resynchronises at the next first-record pointer.
  const size_t hole = expected.sectors[20].front();
  breakSectorHeader(hole);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 40; ++id) {
    const std::vector<size_t> &span = expected.sectors[id];
    const bool touchesHole = std::find(span.begin(), span.end(), hole) != span.end();
    CHECK(readable(pool, id) == !touchesHole);
  }
  CHECK(!readable(pool, 20));
  // The ring is not cut at the hole: newer records survive, appends continue.
  CHECK(readable(pool, 40) && appendShot(pool, 41) && readable(pool, 41));
  // A torn first-record pointer with an intact header: records chained from
  // the previous sector are kept.
  const size_t tornFirst = expected.sectors[30].front();
  Flash::sector(physicalOf(tornFirst))[offsetof(ShotTracePoolSectorHeader, firstRecord)] ^= 0x10;
  CHECK(pool.recover());
  CHECK(readable(pool, 30) && readable(pool, 41));
  // A broken oldest sector moves the tail; nothing before it was required.
  breakSectorHeader(0);
  CHECK(pool.recover());
  CHECK(!readable(pool, 1) && readable(pool, 41));
  // A sequence gap between two valid neighbours (never written by the pool)
  // breaks a chain like a hole: the record spanning it is dropped.
  freshFlash();
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 12; ++id) CHECK(appendShot(pool, id));
  startSectors(pool, ids(1, 12), expected);
  const std::vector<size_t> span = expected.sectors[4];  // 22 KB: six sectors
  for (size_t s = span[3]; s <= newestEndSector(pool); ++s) {
    auto *h = reinterpret_cast<ShotTracePoolSectorHeader *>(Flash::sector(physicalOf(s)));
    ShotTracePoolSectorHeader copy = *h;
    ++copy.sequence;
    copy.crc = ~crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&copy),
                            offsetof(ShotTracePoolSectorHeader, crc));
    memset(h, 0xFF, sizeof(*h));
    memcpy(h, &copy, sizeof(copy));
  }
  CHECK(pool.recover());
  CHECK(readable(pool, 3) && !readable(pool, 4) && readable(pool, 12));
  // A sector whose sequence runs backwards is not part of this ring: a hole.
  const size_t stale = expected.sectors[8].front();
  auto *h = reinterpret_cast<ShotTracePoolSectorHeader *>(Flash::sector(physicalOf(stale)));
  ShotTracePoolSectorHeader copy = *h;
  copy.sequence = copy.floor;
  copy.crc = ~crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&copy),
                          offsetof(ShotTracePoolSectorHeader, crc));
  memcpy(h, &copy, sizeof(copy));
  CHECK(pool.recover());
  CHECK(!readable(pool, 8) && readable(pool, 12));
}

void tsp10_corrupt_payload_and_record_header() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 6; ++id) CHECK(appendShot(pool, id));
  ShotTracePoolEntry e;
  CHECK(pool.entryAt(2, e));
  // One payload bit: found, but CRC-checked on open.
  const size_t at = e.address() + 200U;
  Flash::sector(physicalOf(at / S))[at % S] ^= 0x01;
  bool matches = false;
  CHECK(readShot(pool, 3, matches) == ShotTracePoolStatus::CORRUPT);
  CHECK(pool.recover());  // Boot does not check payloads.
  CHECK(pool.contains(3, identityOf(3).bootId, identityOf(3).endedAtMs));
  CHECK(readShot(pool, 3, matches) == ShotTracePoolStatus::CORRUPT);
  CHECK(readable(pool, 2) && readable(pool, 4));
  // A broken record magic ends that chain; later sectors resynchronise.
  CHECK(pool.entryAt(1, e));
  Flash::sector(physicalOf(e.address() / S))[e.address() % S] ^= 0x01;
  CHECK(pool.recover());
  CHECK(readable(pool, 1) && !readable(pool, 2));
  CHECK(readable(pool, 6));
}

void tsp11_linkage_refuses_other_boots_and_times() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  CHECK(appendShot(pool, 5));
  const ShotTracePoolIdentity identity = identityOf(5);
  ShotTracePoolRef ref;
  uint8_t buffer[128];
  CHECK(pool.contains(5, identity.bootId, identity.endedAtMs));
  CHECK(!pool.contains(5, identity.bootId + 1U, identity.endedAtMs));
  CHECK(!pool.contains(5, identity.bootId, identity.endedAtMs + 1U));
  CHECK(pool.open(5, identity.bootId + 1U, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::NOT_FOUND);
  CHECK(pool.open(5, identity.bootId, identity.endedAtMs + 1U, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::NOT_FOUND);
  // A tag collision (same 24-bit id and tag) is still refused on flash.
  uint32_t otherBoot = identity.bootId + 1U;
  while (shotTracePoolIdTag(5, otherBoot, identity.endedAtMs) !=
         shotTracePoolIdTag(5, identity.bootId, identity.endedAtMs)) ++otherBoot;
  CHECK(!pool.contains(5, otherBoot, identity.endedAtMs));
  CHECK(pool.open(5, otherBoot, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::NOT_FOUND);
  // Id reuse without a pool clear (a downgrade): a newer colliding record
  // does not hide the older one, and neither answers for a third identity.
  CHECK(pool.append({5, otherBoot, identity.endedAtMs, 1, 0}, nullptr, 0));
  uint32_t thirdBoot = otherBoot + 1U;
  while (shotTracePoolIdTag(5, thirdBoot, identity.endedAtMs) !=
         shotTracePoolIdTag(5, identity.bootId, identity.endedAtMs)) ++thirdBoot;
  CHECK(readable(pool, 5));
  CHECK(pool.contains(5, otherBoot, identity.endedAtMs));
  CHECK(!pool.contains(5, thirdBoot, identity.endedAtMs));
  // Id reuse after a clear: the new shot never meets the old record.
  CHECK(pool.clear());
  CHECK(pool.open(5, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::NOT_FOUND);
  CHECK(pool.append({5, 9999, 42, 1, 0}, nullptr, 0));
  CHECK(pool.recover());
  CHECK(pool.open(5, 9999, 42, ref, buffer, sizeof(buffer)) == ShotTracePoolStatus::OK);
  CHECK(pool.open(5, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::NOT_FOUND);
}

void tsp12_clear_hides_everything_and_survives_reboot() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 30; ++id) CHECK(appendShot(pool, id));
  const size_t ops = Flash::operations();
  CHECK(pool.clear());
  CHECK(Flash::operations() - ops == 2);  // One erase, one header.
  CHECK(pool.count() == 0);
  CHECK(pool.recover() && pool.count() == 0);
  CHECK(appendShot(pool, 31) && readable(pool, 31));
  CHECK(pool.recover() && pool.count() == 1 && readable(pool, 31) && !readable(pool, 30));
  // Clear before any recover (factory reset at boot) recovers first.
  ShotTracePool cold(indexB, RESERVED);
  CHECK(cold.clear() && cold.count() == 0);
  CHECK(pool.recover() && pool.count() == 0);
  // A cleared pool never resurrects older sectors after the ring wraps back.
  for (uint32_t id = 100; id <= 200; ++id) CHECK(appendShot(pool, id));
  CHECK(pool.recover() && !readable(pool, 30) && readable(pool, 200));
}

bool flashWasRead = false;
void noteFlashRead() { flashWasRead = true; }

struct LiveSet {
  std::set<uint32_t> ids;
};
bool isLive(void *context, const ShotTracePoolEntry &entry) {
  for (uint32_t id : static_cast<LiveSet *>(context)->ids) {
    const ShotTracePoolIdentity identity = identityOf(id);
    if (shotTracePoolMatches(entry, id, identity.bootId, identity.endedAtMs)) return true;
  }
  return false;
}

void tsp13_stats() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  ShotTracePoolStats stats = pool.stats(nullptr, nullptr);
  CHECK(stats.poolBytes == RESERVED * S && stats.unusedBytes == RESERVED * S);
  CHECK(stats.records == 0 && stats.lastError == ShotTracePoolError::NONE);
  ShotTracePoolRef oldest;
  CHECK(pool.headerAt(0, oldest) == ShotTracePoolStatus::NOT_FOUND);
  for (uint32_t id = 1; id <= 8; ++id) CHECK(appendShot(pool, id));
  LiveSet live{{2, 3, 4, 5, 6, 7, 8}};  // Shot 1 was deleted from the log.
  stats = pool.stats(isLive, &live);
  ShotTracePoolEntry first;
  CHECK(pool.entryAt(0, first));
  CHECK(stats.records == 8 && stats.droppedRecords == 0);
  CHECK(stats.reclaimableBytes == first.bytes());
  uint32_t sum = 0;
  for (const ShotTracePoolEntry &e : entries(pool)) sum += e.bytes();
  CHECK(stats.liveBytes + stats.reclaimableBytes == sum);
  CHECK(stats.liveBytes + stats.reclaimableBytes + stats.unindexedBytes + stats.unusedBytes ==
        stats.poolBytes);
  CHECK(stats.meanRecordBytes == sum / 8U);
  // stats() is RAM only; a record's identity is a separate header read.
  flashWasRead = false;
  Flash::setReadHook(noteFlashRead);
  (void)pool.stats(isLive, &live);
  CHECK(!flashWasRead);
  Flash::setReadHook(nullptr);
  CHECK(pool.headerAt(0, oldest) == ShotTracePoolStatus::OK && oldest.header.shotId == 1 &&
        oldest.header.bootId == identityOf(1).bootId &&
        oldest.header.endedAtMs == identityOf(1).endedAtMs && oldest.bytes == first.bytes());
  CHECK(pool.headerAt(7, oldest) == ShotTracePoolStatus::OK && oldest.header.shotId == 8 &&
        pool.headerAt(8, oldest) == ShotTracePoolStatus::NOT_FOUND);
  const uint32_t used = newestEndSector(pool) + 1U;
  CHECK(stats.unusedBytes == (RESERVED - used) * S);
  // The same after a reboot.
  CHECK(pool.recover());
  const ShotTracePoolStats again = pool.stats(isLive, &live);
  CHECK(again.unusedBytes == stats.unusedBytes && again.liveBytes == stats.liveBytes);
  // Errors are reported.
  ShotTracePoolSource huge[2] = {{ShotTracePoolSection::TRACE, 0, 16384, traceBuffer, nullptr, nullptr},
                                 {ShotTracePoolSection::CURVE, 0, 8200, traceBuffer, nullptr, nullptr}};
  CHECK(!pool.append(identityOf(9), huge, 2));
  CHECK(pool.stats(nullptr, nullptr).lastError == ShotTracePoolError::TOO_LARGE);
  Flash::setFailAfter(0);
  CHECK(!appendShot(pool, 9));
  Flash::setFailAfter(-1);
  CHECK(pool.lastError() == ShotTracePoolError::PROGRAM || pool.lastError() == ShotTracePoolError::ERASE);
}

void tsp14_uncommitted_record_is_never_returned() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  CHECK(appendShot(pool, 1));
  // Write all of record 2 but its commit word, then reboot.
  const Sizes sizes = sizesOf(2);
  for (size_t i = 0; i < sizes.trace; ++i) traceBuffer[i] = contentByte(2, 2, i);
  for (size_t i = 0; i < sizes.curve; ++i) curveBuffer[i] = contentByte(2, 1, i);
  ShotTracePoolSource sources[2] = {{ShotTracePoolSection::CURVE, 0, sizes.curve, curveBuffer, nullptr, nullptr},
                                    {ShotTracePoolSection::TRACE, 0, sizes.trace, traceBuffer, nullptr, nullptr}};
  CHECK(pool.beginAppend(identityOf(2), sources, 2));
  CHECK(!pool.beginAppend(identityOf(3), sources, 2));  // One at a time.
  size_t steps = 0;
  const size_t before = Flash::operations();
  while (pool.appendStep() == FlashStoreStepResult::MORE) ++steps;
  const size_t all = Flash::operations() - before;
  CHECK(pool.recover());
  CHECK(readable(pool, 2));  // The full run committed it.
  freshFlash();
  CHECK(pool.recover());
  CHECK(appendShot(pool, 1));
  CHECK(pool.beginAppend(identityOf(2), sources, 2));
  for (size_t n = 0; n + 1U < all; ++n) CHECK(pool.appendStep() == FlashStoreStepResult::MORE);
  CHECK(pool.appending());
  pool.cancelAppend();
  ShotTracePool rebooted(indexB, RESERVED);
  CHECK(rebooted.recover());
  CHECK(rebooted.count() == 1 && !readable(rebooted, 2) && readable(rebooted, 1));
  // The torn tail is not reused: the next record starts in the sector after
  // the last one record 2 reached.
  ShotTracePoolEntry e1;
  CHECK(rebooted.entryAt(0, e1));
  const size_t tornEnd = endSector(e1.address() + e1.bytes(), 32U + 4U + sizes.curve + 4U + sizes.trace);
  CHECK(appendShot(rebooted, 3));
  ShotTracePoolEntry e3;
  CHECK(rebooted.entryAt(1, e3));
  CHECK(e3.address() == (tornEnd + 1U) * S + sizeof(ShotTracePoolSectorHeader));
  CHECK(readable(rebooted, 3) && rebooted.recover() && readable(rebooted, 3));
  CHECK(steps + 1U == all);
}

// A flash error that is not a power loss, at every operation of an append:
// the pool keeps working in the same boot, and what it appends afterwards
// survives the next boot.
void tsp16_transient_errors_keep_the_pool_consistent() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  for (uint32_t id = 1; id <= 3; ++id) CHECK(appendShot(pool, id));
  std::vector<uint8_t> image(Flash::image(), Flash::image() + Flash::imageBytes());
  const size_t before = Flash::operations();
  CHECK(appendShot(pool, 4));
  const size_t total = Flash::operations() - before;
  for (size_t f = 0; f < total; ++f) {
    for (size_t torn : {size_t(0), size_t(5), size_t(30)}) {
      memcpy(Flash::image(), image.data(), image.size());
      Flash::setFailAfter(-1);
      CHECK(pool.recover());
      Flash::setFailAfter(static_cast<int>(f), torn, true);
      // Only a commit that landed in full (read back) counts as appended.
      const bool appended = appendShot(pool, 4);
      CHECK(appended ? readable(pool, 4) : pool.lastError() != ShotTracePoolError::NONE);
      CHECK(appendShot(pool, 5) && appendShot(pool, 6));
      CHECK(readable(pool, 5) && readable(pool, 6));
      CHECK(pool.recover());
      bool matches = false;
      const ShotTracePoolStatus status = readShot(pool, 4, matches);
      CHECK(status == ShotTracePoolStatus::NOT_FOUND || (status == ShotTracePoolStatus::OK && matches));
      for (uint32_t id : {1U, 2U, 3U, 5U, 6U}) CHECK(readable(pool, id));
      if (appended) CHECK(readable(pool, 4));
    }
  }
}

// More records on flash than the index holds: appends and recovery keep the
// newest SHOT_TRACE_POOL_INDEX_CAPACITY, the dropped ones read as gone, and
// the statistics count them as unindexed space.
ShotTracePoolIdentity tinyIdentity(uint32_t id) { return {id, 3000U + id % 3U, id * 7U, 1, 0}; }
bool tinyIndexed(ShotTracePool &pool, uint32_t id) {
  const ShotTracePoolIdentity identity = tinyIdentity(id);
  return pool.contains(id, identity.bootId, identity.endedAtMs);
}

void tsp18_full_index_keeps_the_newest_records() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  const uint32_t cap = SHOT_TRACE_POOL_INDEX_CAPACITY, total = cap + 552U;
  for (uint32_t id = 1; id < total; ++id) CHECK(pool.append(tinyIdentity(id), nullptr, 0));
  // The oldest indexed record is opened, then one append drops it.
  const uint32_t oldest = total - cap;
  ShotTracePoolRef ref;
  uint8_t buffer[64];
  CHECK(pool.open(oldest, tinyIdentity(oldest).bootId, tinyIdentity(oldest).endedAtMs, ref, buffer,
                  sizeof(buffer)) == ShotTracePoolStatus::OK);
  CHECK(pool.append(tinyIdentity(total), nullptr, 0));
  CHECK(pool.read(ref, 0, buffer, 32) == ShotTracePoolStatus::GONE);
  CHECK(pool.open(oldest, tinyIdentity(oldest).bootId, tinyIdentity(oldest).endedAtMs, ref, buffer,
                  sizeof(buffer)) == ShotTracePoolStatus::NOT_FOUND);
  CHECK(pool.count() == cap);
  ShotTracePoolEntry e;
  CHECK(pool.entryAt(0, e) && (e.idTag & 0xFFFFFFU) == oldest + 1U);
  CHECK(pool.entryAt(cap - 1U, e) && (e.idTag & 0xFFFFFFU) == total);
  for (uint32_t id = 1; id <= total; ++id) CHECK(tinyIndexed(pool, id) == (id > oldest));
  ShotTracePoolStats stats = pool.stats(nullptr, nullptr);
  CHECK(stats.records == cap && stats.droppedRecords == oldest);
  CHECK(stats.reclaimableBytes == cap * 32U && stats.liveBytes == 0);
  CHECK(stats.unindexedBytes >= oldest * 32U);
  CHECK(stats.liveBytes + stats.reclaimableBytes + stats.unindexedBytes + stats.unusedBytes ==
        stats.poolBytes);
  // Recovery walks oldest first and keeps the same newest records.
  const std::vector<ShotTracePoolEntry> before = entries(pool);
  ShotTracePool rebooted(indexB, RESERVED);
  CHECK(rebooted.recover());
  CHECK(sameEntries(before, entries(rebooted)));
  CHECK(rebooted.stats(nullptr, nullptr).droppedRecords == oldest);
  CHECK(rebooted.open(total, tinyIdentity(total).bootId, tinyIdentity(total).endedAtMs, ref, buffer,
                      sizeof(buffer)) == ShotTracePoolStatus::OK);
  // FIFO recycling still evicts the indexed records it erases.
  for (uint32_t id = 1; id <= 200; ++id) CHECK(appendShot(rebooted, id));
  CHECK(!tinyIndexed(rebooted, total));
  const size_t live = rebooted.count();
  for (uint32_t id = 1; id <= 200; ++id) CHECK(readable(rebooted, id) == (id > 200U - live));
  // Appends into the still-full index dropped more of the oldest records.
  CHECK(rebooted.stats(nullptr, nullptr).droppedRecords > oldest);
  CHECK(pool.recover() && sameEntries(entries(pool), entries(rebooted)));
}

// The writer recycles a record's sector while a reader is inside read():
// the read reports GONE instead of returning the new bytes.
ShotTracePool *recyclingPool = nullptr;
uint32_t nextRecyclingId = 500;
void recycleShotOne() {
  ShotTracePoolEntry oldest;
  while (recyclingPool->entryAt(0, oldest) && (oldest.idTag & 0xFFFFFFU) == 1U)
    CHECK(appendShot(*recyclingPool, nextRecyclingId++));
}

void tsp17_reads_revalidate_against_recycling() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  fillUntil(pool, 1, RESERVED - 8U);
  const ShotTracePoolIdentity identity = identityOf(1);
  ShotTracePoolRef ref;
  uint8_t buffer[256];
  size_t offset = 0, bytes = 0;
  CHECK(pool.open(1, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::OK);
  CHECK(pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) == ShotTracePoolStatus::OK);
  recyclingPool = &pool;
  Flash::setReadHook(recycleShotOne);
  CHECK(pool.read(ref, offset, buffer, sizeof(buffer)) == ShotTracePoolStatus::GONE);
  CHECK(pool.read(ref, offset, buffer, sizeof(buffer)) == ShotTracePoolStatus::GONE);
  CHECK(!readable(pool, 1) && readable(pool, nextRecyclingId - 1U));
}


// An interrupted erase of an older sector can leave a stale record header
// whose length bits partly erased (0 -> 1) while magic and commit survive.
// The next sector's first-record pointer wins: records starting there are
// kept, and the suspect record is dropped.
void tsp19_first_record_pointer_outranks_a_stale_length() {
  for (uint16_t lowest : {uint16_t(0x10), uint16_t(0x1000)}) {  // Lands inside / past the next sector.
    freshFlash();
    ShotTracePool pool(indexA, RESERVED);
    CHECK(pool.recover());
    for (uint32_t id = 1; id <= 12; ++id) CHECK(appendShot(pool, id));
    Expected expected;
    startSectors(pool, ids(1, 12), expected);
    uint32_t suspect = 0;
    for (uint32_t id = 1; id < 12 && suspect == 0; ++id)
      if (expected.sectors[id].size() >= 2 && expected.sectors[id + 1U].front() == expected.sectors[id].back())
        suspect = id;
    CHECK(suspect != 0);
    uint8_t *at = Flash::sector(physicalOf(expected.entry[suspect].address() / S)) +
                  expected.entry[suspect].address() % S + offsetof(ShotTracePoolRecordHeader, totalBytes);
    uint16_t length, bit = lowest;
    memcpy(&length, at, sizeof(length));
    while ((length & bit) != 0) bit = static_cast<uint16_t>(bit << 1);
    CHECK((length | bit) <= SHOT_TRACE_POOL_MAX_RECORD_BYTES);
    length = static_cast<uint16_t>(length | bit);
    memcpy(at, &length, sizeof(length));
    CHECK(pool.recover());
    for (uint32_t id = 1; id <= 12; ++id) CHECK(readable(pool, id) == (id != suspect));
    CHECK(appendShot(pool, 13) && pool.recover() && readable(pool, 13));
  }
}

// Records leave the index before their sector is erased: a reader can never
// pass both revalidations of a record whose bytes are being erased.
ShotTracePool *spiedPool = nullptr;
bool erasedIndexedSector = false;
void spyErase(size_t physical) {
  const size_t logical = physical < RESERVED ? physical : physical - 2U;
  ShotTracePoolEntry e;
  for (size_t n = 0; spiedPool->entryAt(n, e); ++n)
    erasedIndexedSector = erasedIndexedSector || e.address() / S == logical;
}

void tsp20_eviction_precedes_every_erase() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  spiedPool = &pool;
  erasedIndexedSector = false;
  Flash::setEraseSpy(spyErase);
  for (uint32_t id = 1; id <= 300; ++id) CHECK(appendShot(pool, id));
  CHECK(pool.clear());
  Flash::setEraseSpy(nullptr);
  CHECK(!erasedIndexedSector);
}

// Exact sizes: one trace section of total - 36 bytes.
bool appendSized(ShotTracePool &pool, uint32_t id, uint16_t total) {
  const uint16_t bytes = static_cast<uint16_t>(total - 36U);
  for (size_t i = 0; i < bytes; ++i) traceBuffer[i] = contentByte(id, 2, i);
  const ShotTracePoolSource source = {ShotTracePoolSection::TRACE, 0, bytes, traceBuffer, nullptr, nullptr};
  return pool.append(tinyIdentity(id), &source, 1);
}
bool readableSized(ShotTracePool &pool, uint32_t id, uint16_t total) {
  ShotTracePoolRef ref;
  uint8_t buffer[512];
  size_t offset = 0, bytes = 0;
  if (pool.open(id, tinyIdentity(id).bootId, tinyIdentity(id).endedAtMs, ref, buffer, sizeof(buffer)) !=
          ShotTracePoolStatus::OK || ref.bytes != total ||
      pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) != ShotTracePoolStatus::OK)
    return false;
  for (size_t at = 0; at < bytes; at += sizeof(buffer)) {
    const size_t piece = bytes - at < sizeof(buffer) ? bytes - at : sizeof(buffer);
    if (pool.read(ref, offset + at, buffer, piece) != ShotTracePoolStatus::OK) return false;
    for (size_t i = 0; i < piece; ++i)
      if (buffer[i] != contentByte(id, 2, at + i)) return false;
  }
  return true;
}
uint32_t addressOf(ShotTracePool &pool, size_t n) {
  ShotTracePoolEntry e;
  return pool.entryAt(n, e) ? e.address() : UINT32_MAX;
}

void tsp21_sector_boundaries() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  const struct {
    uint16_t total;
    uint32_t address;
  } plan[] = {
      {4032, 24},                // ends at 4056
      {48, 4056},                // the last offset a header fits; spans into sector 1
      {4064, S + 32},            // ends exactly at offset 4096
      {4048, 2 * S + 24},        // the next starts the next sector; ends at 4072
      {64, 3 * S + 24},          // 4072 leaves no room for a header
      {8080, 3 * S + 88},        // spans and ends exactly at 4096 of sector 4
      {48, 5 * S + 24},
  };
  for (uint32_t n = 0; n < 7; ++n) {
    CHECK(appendSized(pool, 700 + n, plan[n].total));
    CHECK(addressOf(pool, n) == plan[n].address);
    if (n == 2) {  // The newest record ends exactly at a sector end.
      CHECK(pool.recover() && readableSized(pool, 702, plan[2].total));
      CHECK(addressOf(pool, 2) == plan[2].address);
    }
  }
  const std::vector<ShotTracePoolEntry> before = entries(pool);
  CHECK(pool.recover() && sameEntries(before, entries(pool)));
  for (uint32_t n = 0; n < 7; ++n) CHECK(readableSized(pool, 700 + n, plan[n].total));
  // A record ending exactly at 4096 of the next sector, then power lost
  // right after the following sector's header (first record at 24) landed:
  // that head holds no committed record, so appends start a new sector.
  CHECK(appendSized(pool, 707, 8096) && addressOf(pool, 7) == 5 * S + 72);
  const ShotTracePoolSource none = {ShotTracePoolSection::TRACE, 0, 12, traceBuffer, nullptr, nullptr};
  CHECK(pool.beginAppend(tinyIdentity(708), &none, 1));
  CHECK(pool.appendStep() == FlashStoreStepResult::MORE);  // Erase sector 7.
  CHECK(pool.appendStep() == FlashStoreStepResult::MORE);  // Its header, first record 24.
  pool.cancelAppend();                                     // Power lost.
  CHECK(pool.recover() && readableSized(pool, 707, 8096));
  CHECK(appendSized(pool, 709, 48) && addressOf(pool, 8) == 8 * S + 24);
  CHECK(pool.recover() && readableSized(pool, 709, 48));
}

// A dead reference costs no flash read.
void tsp22_dead_reference_reads_nothing() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && appendShot(pool, 1));
  ShotTracePoolRef ref;
  uint8_t buffer[64];
  CHECK(pool.open(1, identityOf(1).bootId, identityOf(1).endedAtMs, ref, buffer, sizeof(buffer)) ==
        ShotTracePoolStatus::OK);
  CHECK(pool.recover());  // Every earlier reference is retired.
  flashWasRead = false;
  Flash::setReadHook(noteFlashRead);
  CHECK(pool.read(ref, 0, buffer, sizeof(buffer)) == ShotTracePoolStatus::GONE);
  CHECK(!flashWasRead);
  Flash::setReadHook(nullptr);
}

// After a crash, appending continues in the head sector only after a
// committed record and only where its first-record pointer is programmable.
void tsp23_append_position_after_a_crash() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && appendShot(pool, 3));  // 12 KB: spans sectors 0-2.
  ShotTracePoolEntry r;
  CHECK(pool.entryAt(0, r));
  const size_t head = endSector(r.address(), r.bytes());
  CHECK(head >= 2);
  // A torn first-record program in the head (power lost during the next
  // append): neither blank nor valid.
  uint32_t first;
  uint8_t *field = Flash::sector(physicalOf(head)) + offsetof(ShotTracePoolSectorHeader, firstRecord);
  memcpy(&first, field, sizeof(first));
  CHECK(first == UINT32_MAX);
  const uint32_t offset = (r.address() + r.bytes()) % S;  // Not exact: any torn value works.
  first = (offset | (~offset & 0xFFFFU) << 16) | 0x00F00000U;
  memcpy(field, &first, sizeof(first));
  CHECK(pool.recover() && readable(pool, 3));
  CHECK(appendShot(pool, 8));
  CHECK(addressOf(pool, 1) == (head + 1U) * S + 24U);
  // Once the chain into that sector is gone, the record still resynchronises.
  breakSectorHeader(head - 1U);
  CHECK(pool.recover() && readable(pool, 8) && !readable(pool, 3));
  // Bytes after the newest record that are not blank (a torn program whose
  // header bits did not land): the next record starts a new sector.
  CHECK(appendShot(pool, 16));
  ShotTracePoolEntry newest;
  CHECK(pool.entryAt(pool.count() - 1U, newest));
  const size_t tail = endSector(newest.address(), newest.bytes());
  const size_t after = (newest.address() + newest.bytes()) % S;
  CHECK(tail == newest.address() / S && after + 200U < S);
  Flash::sector(physicalOf(tail))[after + 100U] = 0x00;
  CHECK(pool.recover() && appendShot(pool, 17));
  CHECK(pool.entryAt(pool.count() - 1U, newest) && newest.address() == (tail + 1U) * S + 24U);
  CHECK(readable(pool, 17) && pool.recover() && readable(pool, 17));
  // A head holding no committed record (here the clear marker) is not
  // appended to after a reboot.
  CHECK(pool.clear() && pool.recover() && appendShot(pool, 9));
  const size_t sector = addressOf(pool, 0) / S;
  const auto *marker = reinterpret_cast<const ShotTracePoolSectorHeader *>(Flash::sector(physicalOf(sector - 1U)));
  CHECK(addressOf(pool, 0) % S == 24 && marker->floor == marker->sequence);
  CHECK(pool.recover() && readable(pool, 9));
}

// A full index with a record spanning into a sector that never joined: the
// newest complete records stay, counted once.
void tsp24_full_index_and_an_unconfirmed_span() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  const uint32_t cap = SHOT_TRACE_POOL_INDEX_CAPACITY, tiny = cap + 100U;
  for (uint32_t id = 1; id <= tiny; ++id) CHECK(pool.append(tinyIdentity(id), nullptr, 0));
  CHECK(appendShot(pool, 4));  // 22 KB, the newest, spanning six sectors.
  ShotTracePoolEntry big;
  CHECK(pool.entryAt(cap - 1U, big));
  breakSectorHeader(endSector(big.address(), big.bytes()));
  CHECK(pool.recover());
  CHECK(pool.count() == cap && pool.stats(nullptr, nullptr).droppedRecords == tiny - cap);
  ShotTracePoolEntry e;
  CHECK(pool.entryAt(cap - 1U, e) && (e.idTag & 0xFFFFFFU) == tiny);
  CHECK(pool.entryAt(0, e) && (e.idTag & 0xFFFFFFU) == tiny - cap + 1U);
  CHECK(!pool.contains(4, identityOf(4).bootId, identityOf(4).endedAtMs));
}

// Two power losses in a row: one during an append, the next during the first
// append after that recovery, with every tear model.
void tsp25_crash_during_the_first_append_after_recovery() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  std::vector<uint32_t> prefill;
  for (uint32_t id = 8; prefill.size() < 30; ++id)
    if (id % 8 == 0 || id % 8 == 1 || id % 8 == 5) prefill.push_back(id);
  for (uint32_t id : prefill) CHECK(appendShot(pool, id));
  const std::vector<uint8_t> image(Flash::image(), Flash::image() + RESERVED * S);
  size_t runs = 0;
  for (int first : {2, 7, 15}) {
    for (int second = 0; second < 40; ++second) {
      for (size_t torn : {size_t(5), Flash::kRandomTear}) {
        memcpy(Flash::image(), image.data(), image.size());
        Flash::setFailAfter(-1);
        CHECK(pool.recover());
        Flash::setFailAfter(first, torn, false, static_cast<uint32_t>(second * 31 + first));
        CHECK(!appendShot(pool, 604));  // 22 KB: more than 15 operations.
        Flash::setFailAfter(-1);
        CHECK(pool.recover());
        Flash::setFailAfter(second, torn, false, static_cast<uint32_t>(second * 37 + first));
        const bool committed = appendShot(pool, 603);
        Flash::setFailAfter(-1);
        CHECK(pool.recover());
        for (uint32_t id : prefill) CHECK(readable(pool, id));
        bool matches = false;
        for (uint32_t id : {603U, 604U}) {
          const ShotTracePoolStatus status = readShot(pool, id, matches);
          CHECK(status == ShotTracePoolStatus::NOT_FOUND || (status == ShotTracePoolStatus::OK && matches));
        }
        if (committed) CHECK(readable(pool, 603));
        CHECK(appendShot(pool, 616) && pool.recover() && readable(pool, 616));
        ++runs;
      }
    }
  }
  CHECK(runs == 240);
}

void tsp15_concurrent_reader_and_writer() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  std::atomic<uint32_t> newest{0};
  std::atomic<bool> done{false};
  std::atomic<int> ok{0}, gone{0}, bad{0};
  // The reader chases the oldest records, which the writer keeps recycling.
  std::thread reader([&] {
    uint32_t round = 0;
    while (!done.load()) {
      ShotTracePoolEntry e;
      if (newest.load() == 0 || !pool.entryAt(round++ % 4U, e)) continue;
      const uint32_t id = e.idTag & 0xFFFFFFU;
      const ShotTracePoolIdentity identity = identityOf(id);
      ShotTracePoolRef ref;
      uint8_t buffer[512];
      const ShotTracePoolStatus status =
          pool.open(id, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer));
      if (status == ShotTracePoolStatus::OK) {
        size_t offset, bytes;
        ShotTracePoolStatus read = pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes);
        for (size_t at = 0; read == ShotTracePoolStatus::OK && at < bytes; at += sizeof(buffer)) {
          const size_t piece = bytes - at < sizeof(buffer) ? bytes - at : sizeof(buffer);
          read = pool.read(ref, offset + at, buffer, piece);
          for (size_t i = 0; read == ShotTracePoolStatus::OK && i < piece; ++i)
            if (buffer[i] != contentByte(id, 2, at + i)) bad.fetch_add(1);
        }
        (read == ShotTracePoolStatus::OK ? ok : gone).fetch_add(1);
      } else if (status == ShotTracePoolStatus::GONE || status == ShotTracePoolStatus::NOT_FOUND) {
        gone.fetch_add(1);
      } else {
        bad.fetch_add(1);
      }
      (void)pool.stats(nullptr, nullptr);
    }
  });
  for (uint32_t id = 1; id <= 400; ++id) {
    CHECK(appendShot(pool, id));
    newest.store(id);
  }
  done.store(true);
  reader.join();
  CHECK(bad.load() == 0);
  CHECK(ok.load() > 0 && gone.load() > 0);
  std::cout << "TSP15: " << ok.load() << " reads, " << gone.load() << " recycled or missing\n";
}

// Readers race the owner's recover(), clear() and setLogicalEnd(): they see
// NOT_READY, NOT_FOUND or GONE, never other data.
void tsp26_readers_race_recovery_clear_and_end_changes() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover());
  std::atomic<bool> done{false};
  std::atomic<int> ok{0}, bad{0};
  std::thread reader([&] {
    uint8_t buffer[512];
    for (uint32_t round = 0; !done.load(); ++round) {
      std::this_thread::yield();  // Let the writer progress under TSAN.
      ShotTracePoolEntry e;
      ShotTracePoolRef oldest;
      if (round % 16U == 0) (void)pool.stats(nullptr, nullptr);
      if (pool.headerAt(0, oldest) == ShotTracePoolStatus::OK &&
          oldest.header.bootId != tinyIdentity(oldest.header.shotId).bootId)
        bad.fetch_add(1);
      if (!pool.entryAt(round % 8U, e)) continue;
      const uint32_t id = e.idTag & 0xFFFFFFU;
      const ShotTracePoolIdentity identity = tinyIdentity(id);
      (void)pool.contains(id, identity.bootId, identity.endedAtMs);
      ShotTracePoolRef ref;
      const ShotTracePoolStatus status =
          pool.open(id, identity.bootId, identity.endedAtMs, ref, buffer, sizeof(buffer));
      if (status == ShotTracePoolStatus::CORRUPT || status == ShotTracePoolStatus::IO_ERROR) bad.fetch_add(1);
      if (status != ShotTracePoolStatus::OK) continue;
      size_t offset, bytes;
      ShotTracePoolStatus read = pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes);
      for (size_t at = 0; read == ShotTracePoolStatus::OK && at < bytes; at += sizeof(buffer)) {
        const size_t piece = bytes - at < sizeof(buffer) ? bytes - at : sizeof(buffer);
        read = pool.read(ref, offset + at, buffer, piece);
        for (size_t i = 0; read == ShotTracePoolStatus::OK && i < piece; ++i)
          if (buffer[i] != contentByte(id, 2, at + i)) bad.fetch_add(1);
      }
      if (read == ShotTracePoolStatus::OK) ok.fetch_add(1);
    }
  });
  // Small records keep the instrumented build fast; 120 KB still wraps
  // nothing, so recover, clear and the end change are the writer's events.
  for (uint32_t id = 1; id <= 80; ++id) {
    CHECK(appendSized(pool, id, static_cast<uint16_t>(1024U + id % 4U * 512U)));
    if (id % 20 == 0) CHECK(pool.recover());
    if (id == 50) CHECK(pool.clear());
    if (id == 60) CHECK(pool.setLogicalEnd(SHOT_TRACE_POOL_SECTORS) && pool.recover());
  }
  done.store(true);
  reader.join();
  CHECK(bad.load() == 0 && ok.load() > 0);
}

// A section read through a callback: bytes that change mid-append abandon
// it (SOURCE), nothing of it is ever committed, and the pool goes on.
struct ChangingSource {
  uint32_t id = 0, failAt = UINT32_MAX, reads = 0;
};
bool readChanging(void *context, size_t offset, uint8_t *out, size_t bytes) {
  ChangingSource &source = *static_cast<ChangingSource *>(context);
  if (source.reads++ >= source.failAt) return false;
  for (size_t i = 0; i < bytes; ++i) out[i] = contentByte(source.id, 2, offset + i);
  return true;
}

void tsp27_a_changing_source_abandons_the_append() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && appendShot(pool, 1));
  ChangingSource changing;
  changing.id = 2;
  const ShotTracePoolSource source = {ShotTracePoolSection::TRACE, 0, 9000, nullptr, readChanging, &changing};
  // Fails during the CRC pass: nothing starts.
  changing.failAt = 3;
  const size_t ops = Flash::operations();
  CHECK(!pool.beginAppend(tinyIdentity(2), &source, 1) && pool.lastError() == ShotTracePoolError::SOURCE);
  CHECK(Flash::operations() == ops && !pool.appending());
  // Fails while being written: abandoned, never committed.
  changing.reads = 0;
  changing.failAt = 40;  // The CRC pass reads 36 chunks of 256 bytes.
  CHECK(pool.beginAppend(tinyIdentity(2), &source, 1));
  FlashStoreStepResult result;
  do { result = pool.appendStep(); } while (result == FlashStoreStepResult::MORE);
  CHECK(result == FlashStoreStepResult::FAILED && pool.lastError() == ShotTracePoolError::SOURCE);
  CHECK(!readableSized(pool, 2, 9040) && pool.recover() && !readableSized(pool, 2, 9040));
  // An unchanged source appends through the callback.
  changing.reads = 0;
  changing.failAt = UINT32_MAX;
  CHECK(pool.append(tinyIdentity(2), &source, 1) && readableSized(pool, 2, 9040));
  CHECK(readable(pool, 1) && pool.recover() && readableSized(pool, 2, 9040));
}

// A commit reported as failed that landed is read back and indexed: the
// record exists once, in this boot and after the next.
void tsp28_a_landed_commit_reported_failed_is_indexed() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && appendShot(pool, 1));
  const size_t before = Flash::operations();
  CHECK(appendShot(pool, 2));
  const size_t ops = Flash::operations() - before;
  freshFlash();
  CHECK(pool.recover() && appendShot(pool, 1));
  Flash::setFailAfter(static_cast<int>(ops - 1U), 4, true);  // The commit lands, reported failed.
  CHECK(appendShot(pool, 2) && pool.count() == 2);
  Flash::setFailAfter(static_cast<int>(ops - 1U), 2, true);  // Half a commit: not committed.
  CHECK(!appendShot(pool, 3) && pool.count() == 2 && !readable(pool, 3));
  CHECK(pool.recover() && pool.count() == 2 && readable(pool, 2));
}

// A flash lock timeout does nothing: the staged append resumes where it
// was, with no erase wasted on a new sector.
void tsp30_a_lock_timeout_resumes_the_staged_append() {
  std::vector<ShotTracePoolEntry> expected;
  size_t erases = 0;
  for (bool timeout : {false, true}) {
    freshFlash();
    ShotTracePool pool(indexA, RESERVED);
    CHECK(pool.recover() && appendShot(pool, 1) && appendShot(pool, 3));
    const Sizes sizes = sizesOf(2);
    for (size_t i = 0; i < sizes.trace; ++i) traceBuffer[i] = contentByte(2, 2, i);
    for (size_t i = 0; i < sizes.curve; ++i) curveBuffer[i] = contentByte(2, 1, i);
    ShotTracePoolSource sources[2] = {
        {ShotTracePoolSection::CURVE, 0, sizes.curve, curveBuffer, nullptr, nullptr},
        {ShotTracePoolSection::TRACE, 0, sizes.trace, traceBuffer, nullptr, nullptr}};
    CHECK(pool.beginAppend(identityOf(2), sources, 2));
    size_t steps = 0;
    for (FlashStoreStepResult result = FlashStoreStepResult::MORE;
         result != FlashStoreStepResult::COMPLETE && steps < 100; ++steps) {
      g_hostFlashIoMutexAvailable = !(timeout && steps == 2);
      result = pool.appendStep();
      g_hostFlashIoMutexAvailable = true;
      if (timeout && steps == 2)
        CHECK(result == FlashStoreStepResult::FAILED && pool.appending() &&
              pool.lastError() == ShotTracePoolError::LOCK);
      // The worker stages again only when nothing is staged.
      if (!pool.appending() && result != FlashStoreStepResult::COMPLETE)
        CHECK(pool.beginAppend(identityOf(2), sources, 2));
    }
    size_t total = 0;
    for (size_t s = 0; s < Flash::kSectors; ++s) total += Flash::erases(s);
    if (!timeout) {
      expected = entries(pool);
      erases = total;
    } else {
      CHECK(sameEntries(entries(pool), expected) && total == erases && readable(pool, 2));
    }
  }
}

// A section longer than its record is damage, never bytes past the record.
void tsp31_a_section_past_its_record_is_corrupt() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && appendShot(pool, 3));
  const ShotTracePoolIdentity id = identityOf(3);
  ShotTracePoolRef ref;
  uint8_t buffer[256];
  size_t offset = 0, bytes = 0;
  CHECK(pool.open(3, id.bootId, id.endedAtMs, ref, buffer, sizeof(buffer)) ==
            ShotTracePoolStatus::OK &&
        pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) ==
            ShotTracePoolStatus::OK);
  ref.bytes = static_cast<uint32_t>(offset + bytes - 1U);
  CHECK(pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) ==
        ShotTracePoolStatus::CORRUPT);
}

// A record without sections (no source array at all) is still a record.
void tsp32_a_record_without_sections() {
  freshFlash();
  ShotTracePool pool(indexA, RESERVED);
  CHECK(pool.recover() && pool.append(identityOf(5), nullptr, 0) && pool.count() == 1);
  const ShotTracePoolIdentity id = identityOf(5);
  ShotTracePoolRef ref;
  uint8_t buffer[64];
  size_t offset = 0, bytes = 0;
  CHECK(pool.open(5, id.bootId, id.endedAtMs, ref, buffer, sizeof(buffer)) ==
            ShotTracePoolStatus::OK &&
        pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) ==
            ShotTracePoolStatus::NOT_FOUND);
}

// The firmware's cutover: the ring opens all of shotcurve over what the
// retired legacy curve store left there (its two epoch sectors and 501
// committed 8 KiB blocks of 36-byte headers and stream bytes). None reads as
// a record, the epoch sectors are never touched, and appends overwrite the
// rest.
void tsp29_legacy_curve_blocks_are_holes() {
  freshFlash();
  uint8_t *legacy = Flash::sector(RESERVED);
  for (size_t at = 0; at < SHOT_TRACE_POOL_SHOTCURVE_BYTES; ++at)
    legacy[at] = static_cast<uint8_t>(at * 31U >> 3);
  const uint32_t epoch[4] = {0x53434550U, 7, 0x1234, 0};
  memcpy(legacy, epoch, sizeof(epoch));
  memcpy(legacy + S, epoch, sizeof(epoch));
  for (uint32_t block = 0; block < 501; ++block) {
    // Magic "SCRV", schema 3 of 5632 bytes, epoch, sequence, floor, shot
    // id, count and flags, checksum, committed.
    const uint32_t header[9] = {0x53435256U, 3U | 5632U << 16, 7, block + 1U, 0, block + 1U,
                                1201U, 0x5555U, UINT32_MAX - 1U};
    memcpy(legacy + 2U * S + block * 8192U, header, sizeof(header));
  }
  ShotTracePool pool(indexA, SHOT_TRACE_POOL_SECTORS);
  CHECK(pool.logicalEnd() == SHOT_TRACE_POOL_SECTORS && pool.recover() && pool.count() == 0);
  CHECK(pool.stats(nullptr, nullptr).unusedBytes == SHOT_TRACE_POOL_SECTORS * S);
  uint32_t id = fillUntil(pool, 1, RESERVED + 40U);
  CHECK(crossesPartitions(pool));
  CHECK(pool.recover() && pool.count() == id - 1U);
  for (uint32_t n = 1; n < id; ++n) CHECK(readable(pool, n));
  CHECK(Flash::touched(LEGACY0) == 0 && Flash::touched(LEGACY1) == 0);
  CHECK(memcmp(Flash::sector(LEGACY0), epoch, sizeof(epoch)) == 0);
}

struct TestCase {
  const char *id;
  void (*function)();
};

const TestCase tests[] = {
    {"TSP01", tsp01_geometry_and_index_budget},
    {"TSP02", tsp02_round_trip_spanning_sectors},
    {"TSP03", tsp03_boot_scan_rebuilds_the_identical_index},
    {"TSP04", tsp04_wrap_evicts_oldest_first},
    {"TSP05", tsp05_shotcurve_opens_and_the_ring_crosses_partitions},
    {"TSP06", tsp06_power_loss_at_every_operation_with_wrap},
    {"TSP07", tsp07_power_loss_across_the_partition_boundary},
    {"TSP08", tsp08_power_loss_during_clear},
    {"TSP09", tsp09_corrupt_sector_headers_are_holes},
    {"TSP10", tsp10_corrupt_payload_and_record_header},
    {"TSP11", tsp11_linkage_refuses_other_boots_and_times},
    {"TSP12", tsp12_clear_hides_everything_and_survives_reboot},
    {"TSP13", tsp13_stats},
    {"TSP14", tsp14_uncommitted_record_is_never_returned},
    {"TSP15", tsp15_concurrent_reader_and_writer},
    {"TSP16", tsp16_transient_errors_keep_the_pool_consistent},
    {"TSP17", tsp17_reads_revalidate_against_recycling},
    {"TSP18", tsp18_full_index_keeps_the_newest_records},
    {"TSP19", tsp19_first_record_pointer_outranks_a_stale_length},
    {"TSP20", tsp20_eviction_precedes_every_erase},
    {"TSP21", tsp21_sector_boundaries},
    {"TSP22", tsp22_dead_reference_reads_nothing},
    {"TSP23", tsp23_append_position_after_a_crash},
    {"TSP24", tsp24_full_index_and_an_unconfirmed_span},
    {"TSP25", tsp25_crash_during_the_first_append_after_recovery},
    {"TSP26", tsp26_readers_race_recovery_clear_and_end_changes},
    {"TSP27", tsp27_a_changing_source_abandons_the_append},
    {"TSP28", tsp28_a_landed_commit_reported_failed_is_indexed},
    {"TSP29", tsp29_legacy_curve_blocks_are_holes},
    {"TSP30", tsp30_a_lock_timeout_resumes_the_staged_append},
    {"TSP31", tsp31_a_section_past_its_record_is_corrupt},
    {"TSP32", tsp32_a_record_without_sections},
};

}  // namespace

int main(int argc, char **argv) {
  int run = 0;
  for (const TestCase &test : tests) {
    if (argc > 1 && std::string(argv[1]) != test.id) continue;
    const int before = failures;
    test.function();
    ++run;
    std::cout << test.id << (failures == before ? " PASS" : " FAIL") << '\n';
  }
  std::cout << run << " shot trace pool tests, " << failures << " failures\n";
  return run != 0 && failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
