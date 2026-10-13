ok, so now we need to make the Windows 11 VM faster and more responsive. Can we minimize the amount of emulation it does? Also, can we set up with COW thing where it gives us the option to save the changes after powering down the VM? Is this feature GUI enabled via virt-manager? If not, we should make it so.

The COW mode must be guarded: the guest may write only to GPT partitions 1, 3, and 7. Writes to every other partition and to disk areas outside those partitions must be rejected while running in the overlay, and changes must remain subject to the same allowlist when saved back to the physical disk.

Change the "Save/Discard Cow changes" so that we can instead have snapshots of changes rather than just one single COW file. I.e. perhaps use the existing snapshot feature to allow for multiple layers of changes, which can be merged back to the physical disk.

Also install the drivers (if they exist) within Windows 11 to help optimize the use within the VM.

## Separate low-memory Windows 11 installation

Evaluate a second Windows 11 installation using the reported low-memory Rufus installation method for Windows 11 26H2:

- https://www.neowin.net/news/you-can-soon-install-tiny-windows-11-26h2-isos-too-on-almost-all-unsupported-pcs/
- https://www.neowin.net/news/windows-11-26h2-reportedly-uses-shockingly-low-ram-with-a-rufus-installation-trick/

Install it to a virtual disk whose GPT partition layout and disk geometry mimic the physical Windows disk closely enough for a fair VM comparison. Both Windows installations will be on the same physical host, but the new VM installation must remain separate from the existing `p3` Windows installation. Do not write the new installation to physical partitions.

The new installation must be activated and properly licensed before it is considered usable or complete. Verify that the existing Windows 11 Pro license allows the VM installation to be used while retaining the `p3` installation, and confirm that Windows reports the VM as activated. Do not assume activation alone proves that the license covers both installations.

If the existing license cannot cover the VM while preserving the existing `p3` installation and its license state, stop before treating the new installation as complete and identify the valid licensing options. Do not remove, overwrite, or invalidate the existing `p3` installation or its license state.

Compare the lightweight installation's boot time, responsiveness, and memory use with the existing Windows 11 VM under the same KVM-backed configuration.
