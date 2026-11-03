#include "machine/OpenBrewByWeightMachineIntegration.h"

#include <cassert>

int main() {
  using namespace openbrewbyweight;
  assert(initializeMachineIntegration());
  publishMachineIntegrationNetworkState(true, false, false);
  requestMachineIntegrationPresetTemperature({2, 935}, 7);
  assert(machineIntegrationPhysicalStart() ==
         MachinePhysicalStartDisposition::NORMAL);
  assert(machineIntegrationBackflush().phase == MachineBackflushPhase::UNSUPPORTED);
  MachineBackflushObservations observations;
  observations.observe(MachineBackflushPhase::AWAITING, 0);
  auto initial = observations.start();
  assert(initial.attempt && !initial.extendable);
  observations.finish(initial.attempt);
  observations.invalidate();
  observations.observe(MachineBackflushPhase::AWAITING, observations.ingressAttempt());
  assert(!observations.start().attempt);  // Recovery cannot reuse a consumed request.
  observations.observe(MachineBackflushPhase::INACTIVE, observations.ingressAttempt());
  observations.observe(MachineBackflushPhase::AWAITING, observations.ingressAttempt());
  auto permit = observations.start();
  assert(permit.extendable && permit.attempt != initial.attempt);
  for (int i = 0; i < 20; ++i)
    observations.observe(MachineBackflushPhase::AWAITING, permit.attempt);
  assert(observations.take().count == 0);
  observations.observe(MachineBackflushPhase::ACTIVE, permit.attempt);
  observations.observe(MachineBackflushPhase::INACTIVE, permit.attempt);
  auto batch = observations.take();
  assert(batch.count == 2 && batch.changes[0].phase == MachineBackflushPhase::ACTIVE &&
         batch.changes[1].phase == MachineBackflushPhase::INACTIVE);
  observations.invalidate();
  observations.observe(MachineBackflushPhase::AWAITING, permit.attempt);
  assert(observations.status().permit.continuity != permit.continuity);
  const auto continuity = observations.status().permit.continuity;
  for (int i = 0; i < 10; ++i)
    observations.observe(i % 2 ? MachineBackflushPhase::ACTIVE : MachineBackflushPhase::INACTIVE,
                         permit.attempt);
  assert(observations.status().permit.continuity != continuity);
  return 0;
}
