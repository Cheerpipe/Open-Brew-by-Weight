#include "nimble/NimbleAdvertisement.h"
#include "ScaleProtocol.h"

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int checks = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " \
                << #condition << std::endl;                                    \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

void accumulateName(const char *name, bool complete, NimbleAdvertisementData &data) {
  nimbleAccumulateAdvertisementName(reinterpret_cast<const uint8_t *>(name),
                                    std::strlen(name), complete, data);
}

void testAdvAndScanResponseMerge() {
  NimbleAdvertisementData data = {};
  nimbleAccumulateAdvertisementUuid16(0xff11, data);
  CHECK(data.compatibleUuid16);
  CHECK(!data.namePresent);
  accumulateName("BOOKOO", true, data);
  CHECK(data.nameComplete);
  CHECK(std::strcmp(data.name, "BOOKOO") == 0);
  CHECK(nimbleAdvertisementIsCompatible(data));
}

void testNameBounds() {
  NimbleAdvertisementData data = {};
  nimbleAccumulateAdvertisementName(nullptr, 4, true, data);
  CHECK(!data.namePresent);
  const std::string longName(38, 'X');
  accumulateName(longName.c_str(), true, data);
  CHECK(data.name[NIMBLE_SCALE_ADV_NAME_CAPACITY - 1] == '\0');
}

void testShortNameCannotReplaceCompleteName() {
  NimbleAdvertisementData data = {};
  accumulateName("BOOKOO", true, data);
  accumulateName("ACA", false, data);
  CHECK(std::strcmp(data.name, "BOOKOO") == 0);
}

void testProtocolInitializationContracts() {
  const uint8_t prohibitedSmoothing[] = {0x03, 0x0a, 0x08,
                                         0x00, 0x00, 0x01};
  bool sawBookoo = false;
  bool sawAcaiaInitialization = false;
  bool sawFelicitaInitialization = false;
  bool sawDifluidInitialization = false;
  for (size_t index = 0; index < scaleProtocolCount(); ++index) {
    const ScaleProtocol *protocol = scaleProtocolAt(index);
    CHECK(protocol != nullptr);
    // A flow hook exists exactly when the protocol declares the capability.
    CHECK((protocol->parseFlowRate != nullptr) ==
          protocol->features.has(ScaleFeatureFlowRate));
    // An in-frame timer declaration needs the hook it exempts from checking.
    CHECK(!protocol->features.has(ScaleFeatureTimerInWeightFrame) ||
          protocol->parseTimer != nullptr);
    // Stock tones come as a pair: both unknown or both non-empty.
    const ScaleStockTones &tones = protocol->stockTones;
    CHECK((tones.powerOn == nullptr) == (tones.powerOff == nullptr));
    CHECK(tones.powerOn == nullptr || (*tones.powerOn && *tones.powerOff));
    if (std::strcmp(protocol->id, "bookoo_generic") == 0) {
      sawBookoo = true;
      CHECK(tones.powerOn != nullptr);
      CHECK(protocol->features.has(ScaleFeatureFlowRate));
      CHECK(protocol->initWrites == nullptr);
      CHECK(protocol->initWriteCount == 0);
    } else if (std::strcmp(protocol->id, "acaia") == 0) {
      sawAcaiaInitialization = protocol->initWriteCount > 0;
    } else if (std::strcmp(protocol->id, "felicita") == 0) {
      sawFelicitaInitialization = protocol->initWriteCount > 0;
    } else if (std::strcmp(protocol->id, "difluid") == 0) {
      sawDifluidInitialization = protocol->initWriteCount > 0;
    }
    for (size_t write = 0; write < protocol->initWriteCount; ++write) {
      const ScalePayload &payload = protocol->initWrites[write];
      CHECK(payload.length != static_cast<int>(sizeof(prohibitedSmoothing)) ||
            std::memcmp(payload.data, prohibitedSmoothing,
                        sizeof(prohibitedSmoothing)) != 0);
    }
    for (uint8_t rawOp = static_cast<uint8_t>(ScaleOp::Tare);
         rawOp <= static_cast<uint8_t>(ScaleOp::SetVolume); ++rawOp) {
      uint8_t command[SCALE_MAX_COMMAND_LENGTH] = {};
      int length = 0;
      if (protocol->encodeCommand(
              static_cast<ScaleOp>(rawOp), 0, command, &length)) {
        CHECK(length != static_cast<int>(sizeof(prohibitedSmoothing)) ||
              std::memcmp(command, prohibitedSmoothing,
                          sizeof(prohibitedSmoothing)) != 0);
      }
    }
  }
  CHECK(sawBookoo);
  CHECK(sawAcaiaInitialization);
  CHECK(sawFelicitaInitialization);
  CHECK(sawDifluidInitialization);
}

void testEurekaWeightFrame() {
  uint8_t frame[11] = {0xaa, 0x09, 0x41, 0, 0, 0, 0, 0x68, 0x01, 0, 0};
  float weight = 0;
  CHECK(kScaleProtocolEureka.parseWeight(frame, sizeof(frame), &weight));
  CHECK(weight == 36.0f);
  frame[6] = 1;
  CHECK(kScaleProtocolEureka.parseWeight(frame, sizeof(frame), &weight));
  CHECK(weight == -36.0f);
  frame[6] = 2;
  CHECK(!kScaleProtocolEureka.parseWeight(frame, sizeof(frame), &weight));
  frame[6] = 0;
  for (int index = 0; index < 3; ++index) {
    const uint8_t original = frame[index];
    frame[index] = 0;
    CHECK(!kScaleProtocolEureka.parseWeight(frame, sizeof(frame), &weight));
    frame[index] = original;
  }
  CHECK(!kScaleProtocolEureka.parseWeight(frame, sizeof(frame) - 1, &weight));
}

void testBookooFlowRateFrame() {
  const ScaleProtocol &bookoo = kScaleProtocolGenericFf11;
  uint8_t frame[20] = {3, 0x0b, 0, 0, 0, 1, '+', 0, 0x0e, 0x10, '+', 0, 0x9d};
  const auto seal = [&frame] {
    frame[19] = 0;
    for (int i = 0; i < 19; ++i) frame[19] ^= frame[i];
  };
  seal();
  float weight = 0;
  float flow = 0;
  CHECK(bookoo.parseWeight(frame, sizeof(frame), &weight) && weight == 36.0f);
  CHECK(bookoo.parseFlowRate(frame, sizeof(frame), &flow) && flow == 1.57f);
  frame[10] = '-';
  seal();
  CHECK(bookoo.parseFlowRate(frame, sizeof(frame), &flow) && flow == -1.57f);
  for (const uint8_t positive : {static_cast<uint8_t>(' '), static_cast<uint8_t>(0)}) {
    frame[10] = positive;
    seal();
    CHECK(bookoo.parseFlowRate(frame, sizeof(frame), &flow) && flow == 1.57f);
  }
  frame[10] = 0x7f;  // An unknown flow sign never invents a flow; weight stays valid.
  seal();
  CHECK(bookoo.parseWeight(frame, sizeof(frame), &weight));
  CHECK(!bookoo.parseFlowRate(frame, sizeof(frame), &flow));
  frame[10] = '+';
  frame[11] = frame[12] = 0xff;
  seal();
  CHECK(bookoo.parseFlowRate(frame, sizeof(frame), &flow) && flow == 655.35f);
  uint32_t timerMs = 0;
  frame[4] = 1;  // Timer is bytes 2-4 in milliseconds, big endian.
  seal();
  CHECK(bookoo.parseTimer(frame, sizeof(frame), &timerMs) && timerMs == 1);
  frame[19] ^= 1;  // One checksum per frame: parseWeight rejects it for all hooks.
  CHECK(bookoo.features.has(ScaleFeatureTimerInWeightFrame));
  CHECK(!bookoo.parseWeight(frame, sizeof(frame), &weight));
  CHECK(!bookoo.parseFlowRate(frame, sizeof(frame) - 1, &flow));
  CHECK(!bookoo.parseTimer(frame, sizeof(frame) - 1, &timerMs));
}

// Byte offsets follow the vendors' published layouts (see the README);
// these are parser-contract fixtures, not hardware captures.
void testBatteryFrames() {
  for (size_t index = 0; index < scaleProtocolCount(); ++index) {
    const ScaleProtocol *protocol = scaleProtocolAt(index);
    CHECK((protocol->parseBattery != nullptr) ==
          protocol->features.has(ScaleFeatureBatteryLevel));
  }
  uint8_t percent = 0;
  uint8_t bookoo[20] = {3, 0x0b};
  bookoo[13] = 62;  // Percent in byte 13 of an accepted weight frame.
  const ScaleProtocol &generic = kScaleProtocolGenericFf11;
  CHECK(generic.parseBattery(bookoo, 20, true, &percent) && percent == 62);
  CHECK(!generic.parseBattery(bookoo, 20, false, &percent));
  CHECK(!generic.parseBattery(bookoo, 19, true, &percent));
  bookoo[13] = 101;
  CHECK(!generic.parseBattery(bookoo, 20, true, &percent));
  uint8_t felicita[18] = {};  // Raw byte 15: 129 empty to 158 full.
  const int curve[][2] = {{129, 0}, {143, 48}, {158, 100}, {200, 100}};
  for (const auto &point : curve) {
    felicita[15] = static_cast<uint8_t>(point[0]);
    CHECK(kScaleProtocolFelicita.parseBattery(felicita, 18, true, &percent) &&
          percent == point[1]);
  }
  CHECK(!kScaleProtocolFelicita.parseBattery(felicita, 18, false, &percent));
  felicita[15] = 128;
  CHECK(!kScaleProtocolFelicita.parseBattery(felicita, 18, true, &percent));
  felicita[15] = 0;  // A frame without the level never reads as empty.
  CHECK(!kScaleProtocolFelicita.parseBattery(felicita, 18, true, &percent));
  // Acaia event 0x06, a standalone frame with its own checksum.
  const ScaleProtocol &acaia = kScaleProtocolAcaia;
  uint8_t event[] = {0xef, 0xdd, 0x0c, 3, 6, 47, 50, 6};
  const uint8_t longer[] = {0xef, 0xdd, 0x0c, 4, 6, 80, 0, 84, 6};
  const uint8_t weight[] = {0xef, 0xdd, 0x0c, 8, 5, 0x68, 1, 0, 0, 1, 0, 0x71, 6};
  CHECK(acaia.supportedPacketLength(8) && acaia.supportedPacketLength(9));
  CHECK(acaia.parseBattery(event, 8, false, &percent) && percent == 47);
  CHECK(acaia.parseBattery(longer, 9, false, &percent) && percent == 80);
  CHECK(!acaia.parseBattery(event, 8, true, &percent));
  CHECK(!acaia.parseBattery(weight, 13, false, &percent));
  event[7] ^= 1;
  CHECK(!acaia.parseBattery(event, 8, false, &percent));
  event[5] = 101;
  event[6] = 104;
  event[7] = 6;
  CHECK(!acaia.parseBattery(event, 8, false, &percent));
  CHECK(kScaleProtocolAcaiaLegacy.parseBattery == nullptr);
}

void fuzzProtocolPacketBounds() {
  for (size_t index = 0; index < scaleProtocolCount(); ++index) {
    const ScaleProtocol *protocol = scaleProtocolAt(index);
    for (int length = 0; length <= SCALE_MAX_PACKET_LENGTH; ++length) {
      std::vector<uint8_t> frame(static_cast<size_t>(length));
      for (uint8_t seed = 0; seed < 16; ++seed) {
        for (int offset = 0; offset < length; ++offset) {
          frame[static_cast<size_t>(offset)] =
              static_cast<uint8_t>(seed * 31U + offset * 17U);
        }
        float weight = 0;
        uint32_t timerMs = 0;
        const bool hasWeight = protocol->parseWeight != nullptr &&
            protocol->parseWeight(frame.data(), length, &weight);
        if (hasWeight) {
          CHECK(std::isfinite(weight));
          CHECK(std::fabs(weight) <= SCALE_MAX_WEIGHT_GRAMS);
        }
        // Every hook must stay in bounds on garbage, even the ones the client
        // only calls after parseWeight accepted the frame.
        if (protocol->parseTimer != nullptr) {
          protocol->parseTimer(frame.data(), length, &timerMs);
        }
        float flow = 0;
        if (protocol->parseFlowRate != nullptr &&
            protocol->parseFlowRate(frame.data(), length, &flow) && hasWeight) {
          CHECK(std::isfinite(flow));
        }
        uint8_t percent = 0;
        for (const bool weightFrame : {false, true}) {
          if (protocol->parseBattery != nullptr &&
              protocol->parseBattery(frame.data(), length, weightFrame, &percent)) {
            CHECK(percent <= 100);
          }
        }
      }
    }
  }
}

void mutateAcceptedProtocolFrames() {
  // Parser-contract fixtures, not hardware captures. Reuse the reviewed
  // Generic FF11 and Eureka layouts; exercise the remaining known grammars.
  // Legacy Acaia and MyScale framing still needs independent device evidence.
  struct Seed {
    const ScaleProtocol *protocol;
    std::vector<uint8_t> bytes;
    int structuralByte;
  };
  const Seed seeds[] = {
      {&kScaleProtocolAcaia, {0xef,0xdd,0x0c,8,5,0x68,1,0,0,1,0,0x71,6}, 0},
      {&kScaleProtocolGenericFf11, {3,0x0b,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8}, 0},
      {&kScaleProtocolFelicita, {0,0,'+','0','0','3','6','0','0','0','0','0','1','0',0,0,0,0}, 2},
      {&kScaleProtocolEclair, {'W',0xa0,0x8c,0,0,0,0,0,0,0x2c}, 0},
      {&kScaleProtocolDecent, {3,0xca,1,0x68,0,0,0xa0}, 0},
      {&kScaleProtocolDifluid, {0xdf,0xdf,3,0,13,0,0,1,0x68,0,0,0,0,0,0,0,0,0,0x37}, 0},
      {&kScaleProtocolWeighMyBru, {3,0x0b,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8}, 0},
      {&kScaleProtocolVaria, {0xfa,1,0,0,0x0e,0x10,0x1f}, 0},
      {&kScaleProtocolEureka, {0xaa,9,0x41,0,0,0,0,0x68,1,0,0}, 0},
  };
  for (const auto &seed : seeds) {
    const auto *protocol=seed.protocol;
    float weight=0;
    const int originalLength=static_cast<int>(seed.bytes.size());
    CHECK(protocol->parseWeight(seed.bytes.data(),originalLength,&weight));
    CHECK(std::isfinite(weight));
    auto malformed=seed.bytes;
    malformed[seed.structuralByte]=0xff;
    CHECK(!protocol->parseWeight(malformed.data(),originalLength,&weight));
    for (int length=0;length<=SCALE_MAX_PACKET_LENGTH;++length) {
      auto frame=seed.bytes;
      frame.resize(static_cast<size_t>(length));
      for (int offset=0;offset<length;++offset) {
        for (unsigned bit=0;bit<8;++bit) {
          const auto original=frame;
          frame[offset]^=static_cast<uint8_t>(1U<<bit);
          for (bool repairIntegrity : {false,true}) {
            if (repairIntegrity && length==originalLength) {
              if (protocol==&kScaleProtocolAcaia) {
                frame[length-2]=frame[length-1]=0;
                for (int i=3;i<length-2;++i) frame[length-2+(i-3)%2]+=frame[i];
              } else if (protocol!=&kScaleProtocolEureka && protocol!=&kScaleProtocolFelicita) {
                frame[length-1]=0;
                const int start=(protocol==&kScaleProtocolEclair || protocol==&kScaleProtocolVaria) ? 1 : 0;
                for (int i=start;i<length-1;++i) {
                  if (protocol==&kScaleProtocolDifluid) frame[length-1]+=frame[i];
                  else frame[length-1]^=frame[i];
                }
              }
            }
            const bool hasWeight=protocol->parseWeight(frame.data(),length,&weight);
            if (hasWeight) {
              CHECK(std::isfinite(weight));
              CHECK(std::fabs(weight)<=SCALE_MAX_WEIGHT_GRAMS);
            }
            uint32_t timer=0;
            if (protocol->parseTimer) protocol->parseTimer(frame.data(),length,&timer);
            float flow=0;
            if (hasWeight && protocol->parseFlowRate &&
                protocol->parseFlowRate(frame.data(),length,&flow)) {
              CHECK(std::isfinite(flow));
            }
          }
          frame=original;
        }
      }
    }
  }
}

}  // namespace

int main() {
  testAdvAndScanResponseMerge();
  testNameBounds();
  testShortNameCannotReplaceCompleteName();
  testProtocolInitializationContracts();
  testEurekaWeightFrame();
  testBookooFlowRateFrame();
  testBatteryFrames();
  fuzzProtocolPacketBounds();
  mutateAcceptedProtocolFrames();
  std::cout << "NimBLE advertisement tests passed: " << checks << " checks\n";
  return 0;
}
