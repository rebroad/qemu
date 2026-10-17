# Goal: improve Windows 11 VM performance

## Objective

Make the existing Windows 11 VM as responsive as possible through KVM acceleration, minimal device emulation, and appropriate guest drivers.

## Scope

Configure the VM to use KVM and available hardware acceleration while minimizing device emulation. Install the relevant VirtIO or other guest drivers that improve the VM's device performance and responsiveness. Validate the effective domain configuration and guest driver state, and compare responsiveness under a consistent workload.

A separate low-memory Windows installation and its controlled comparison are covered by [GOAL-win11-low-memory.md](GOAL-win11-low-memory.md). The guarded physical-disk write boundary is defined in [GOAL-win11.md](GOAL-win11.md).
