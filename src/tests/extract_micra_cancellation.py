"""Compile unchanged Micra cancellation paths without the ESP-IDF TLS stack."""
from pathlib import Path
import re
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()


def definition(name, text=text, source=source):
    # Top-level definitions end at column zero; keep their bodies verbatim.
    match = re.search(r"^(?:const char \*|(?:bool|void|uint8_t|struct|LineaMicraStatus|MachineBackflushSnapshot|MachinePhysicalStartDisposition) )" + re.escape(name)
                      + r"(?:\(| \{)", text, re.M)
    if match is None:
        raise ValueError(f"Missing production definition: {name}")
    end = text.index("\n}", match.start()) + 2
    if text[end:end + 1] == ";":
        end += 1
    line = text.count("\n", 0, match.start()) + 1
    return f'#line {line} "{source.as_posix()}"\n' + text[match.start():end]


names = ["secureWipe", "secureCopy", "powerOptionBit", "sameSessionIdentity",
         "sameCommandRequest", "micraCloudCallResult"]
names += ["OpenBrewByWeightMicraService::" + name for name in
          ["WorkBuffer", "publishConfig", "publishNetworkState", "queue",
           "queueLocked", "muxWaitAnomalyDue", "reportMuxWaitAnomaly",
           "networkEligible",
           "taskLoop", "clearSession", "deferObservation", "executeObservation",
           "observationCurrent", "identityCurrent", "powerRequestCurrent", "publish",
           "scheduleAutomatic", "queueRelist", "executeRelist",
           "selectDiscoveredMachine"]]
output.mkdir(parents=True, exist_ok=True)
(output / "micra_websocket_work.inc").write_text(
    "namespace openbrewbyweight {\nconstexpr size_t kTokenCapacity = 2048;\n"
    + definition("OpenBrewByWeightMicraService::WorkBuffer") + "\n}\n")
(output / "micra_websocket_status.inc").write_text(
    definition("powerOptionBit") + "\n" +
    "\n".join(definition("OpenBrewByWeightMicraService::" + name) for name in
              ("status", "statusLocked", "statusPoll", "statusPollStale",
               "queue", "queueLocked", "muxWaitAnomalyDue",
               "qualifyBackflushLocked", "backflushReadyLocked", "backflush",
               "reportMuxWaitAnomaly", "finishBackflush", "physicalStart",
               "physicalStartLocked", "post", "applyPostedLocked")) + "\n")
(output / "micra_websocket_commands.inc").write_text(
    "\n".join([definition("sameCommandRequest")] +
              [definition("OpenBrewByWeightMicraService::" + name) for name in
               ("temperatureRequestCurrent", "powerRequestCurrent",
                "temperatureEligible", "deferTemperature", "deferPower",
                "executeTemperatureApplication", "executePowerApplication")])
    + "\n")
(output / "micra_cancellation_methods.inc").write_text(
    "namespace openbrewbyweight {\nconstexpr size_t kTokenCapacity = 2048;\n"
    + "\n".join(map(definition, names)) + "\n"
    + definition("OpenBrewByWeightMicraService::websocketAdmitted",
                 source.with_name("OpenBrewByWeightMicraWebSocket.inc").read_text(),
                 source.with_name("OpenBrewByWeightMicraWebSocket.inc")) + "\n}\n")
retry = definition("OpenBrewByWeightMicraService::waitRetry")
(output / "micra_cancellation_retry.inc").write_text(
    retry.split("\n", 1)[0] + "\n" + retry[retry.index("{") + 1:retry.rindex("}")])
request = text.index("bool OpenBrewByWeightMicraService::request(")
gate = text.index("  const auto requestAllowed =", request)
gate_end = text.index("\n  };", gate) + len("\n  };")
loop = text.index("  esp_err_t performed =", request)
loop_end = text.index("  const HeapCapSnapshot heapAfter", loop)
(output / "micra_cancellation_progress.inc").write_text(
    text[gate:gate_end] + "\n" + text[loop:loop_end])
