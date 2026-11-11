"""Compile unchanged Micra cancellation paths without the ESP-IDF TLS stack."""
from pathlib import Path
import re
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()


def definition(name, text=text, source=source):
    # Top-level definitions end at column zero; keep their bodies verbatim.
    match = re.search(r"^(?:const char \*|(?:bool|void|uint8_t|struct|LaMarzoccoError|LaMarzoccoStatus|MachineBackflushSnapshot|MachinePhysicalStartDisposition) )" + re.escape(name)
                      + r"(?:\(| \{)", text, re.M)
    if match is None:
        raise ValueError(f"Missing production definition: {name}")
    end = text.index("\n}", match.start()) + 2
    if text[end:end + 1] == ";":
        end += 1
    line = text.count("\n", 0, match.start()) + 1
    return f'#line {line} "{source.as_posix()}"\n' + text[match.start():end]


names = ["secureWipe", "secureCopy", "powerOptionBit", "sameSessionIdentity",
         "sameCommandRequest", "laMarzoccoCloudCallResult"]
names += ["OpenBrewByWeightLaMarzoccoService::" + name for name in
          ["WorkBuffer", "publishConfig", "publishNetworkState", "queue",
           "queueLocked", "muxWaitAnomalyDue", "reportMuxWaitAnomaly",
           "networkEligible",
           "taskLoop", "clearSession", "discardAccessToken", "deferObservation",
           "executeObservation",
           "observationCurrent", "identityCurrent", "powerRequestCurrent", "publish",
           "scheduleAutomatic", "queueRelist", "executeRelist",
           "selectDiscoveredMachine"]]
output.mkdir(parents=True, exist_ok=True)
(output / "la_marzocco_websocket_work.inc").write_text(
    "namespace openbrewbyweight {\nconstexpr size_t kTokenCapacity = 2048;\n"
    + definition("OpenBrewByWeightLaMarzoccoService::WorkBuffer") + "\n}\n")
(output / "la_marzocco_websocket_status.inc").write_text(
    definition("powerOptionBit") + "\n" + definition("secureCopy") + "\n" +
    "\n".join(definition("OpenBrewByWeightLaMarzoccoService::" + name) for name in
              ("currentStatus", "statusLocked", "liveSubscriptionLocked", "statusPoll", "statusPollStale",
               "mergeReadLocked",
               "queue", "queueLocked", "muxWaitAnomalyDue",
               "qualifyBackflushLocked", "backflushReadyLocked", "backflush",
               "reportMuxWaitAnomaly", "finishBackflush", "physicalStart",
               "physicalStartLocked", "post", "applyPostedLocked", "publish",
               "fail")) + "\n")
(output / "la_marzocco_websocket_commands.inc").write_text(
    "\n".join([definition("sameCommandRequest"), definition("commandFailure")] +
              [definition("OpenBrewByWeightLaMarzoccoService::" + name) for name in
              ("temperatureRequestCurrent", "powerRequestCurrent", "pushConfirms",
               "temperatureEligible", "deferTemperature", "deferPower",
               "discardAccessToken",
               "executeTemperatureApplication", "executePowerApplication")])
    + "\n")
(output / "la_marzocco_cancellation_methods.inc").write_text(
    "namespace openbrewbyweight {\nconstexpr size_t kTokenCapacity = 2048;\n"
    + "\n".join(map(definition, names)) + "\n"
    + definition("OpenBrewByWeightLaMarzoccoService::websocketAdmitted",
                 source.with_name("OpenBrewByWeightLaMarzoccoWebSocket.inc").read_text(),
                 source.with_name("OpenBrewByWeightLaMarzoccoWebSocket.inc")) + "\n}\n")
retry = definition("OpenBrewByWeightLaMarzoccoService::waitRetry")
(output / "la_marzocco_cancellation_retry.inc").write_text(
    retry.split("\n", 1)[0] + "\n" + retry[retry.index("{") + 1:retry.rindex("}")])
request = text.index("bool OpenBrewByWeightLaMarzoccoService::cloudRequest(")
gate = text.index("  const auto requestAllowed =", request)
gate_end = text.index("\n  };", gate) + len("\n  };")
loop = text.index("  esp_err_t performed =", request)
loop_end = text.index("  const HeapCapSnapshot heapAfter", loop)
(output / "la_marzocco_cancellation_progress.inc").write_text(
    text[gate:gate_end] + "\n" + text[loop:loop_end])
