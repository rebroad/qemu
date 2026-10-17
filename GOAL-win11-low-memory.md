# Goal: compare a low-memory Windows 11 installation

## Separate low-memory Windows 11 installation

Evaluate a second Windows 11 installation using the reported low-memory method for Windows 11 26H2. Do not assume Rufus alone creates a stripped-down image: Rufus 4.14's documented Quality of Life option disables selected bundled Microsoft apps, while Tiny11 Builder creates a customized smaller Windows image. The reported memory result is anecdotal and does not establish which change caused it.

Use the same VM hardware profile, VirtIO drivers, Windows update level, startup state, and workload to compare stock Microsoft 26H2 with a Tiny11 Builder **Tiny** image created from official Microsoft media. Record the input ISO hash, builder version/configuration, and removed components. Evaluate Rufus's Quality of Life option separately if testing the reported recipe. Do not use Tiny11 Core: its documented servicing removal would prevent a fair, maintainable Windows Update comparison. Measure idle and workload memory, boot time, and responsiveness multiple times after the same settling period.

The referenced reporting is:

- https://www.neowin.net/news/you-can-soon-install-tiny-windows-11-26h2-isos-too-on-almost-all-unsupported-pcs/
- https://www.neowin.net/news/windows-11-26h2-reportedly-uses-shockingly-low-ram-with-a-rufus-installation-trick/

Install the comparison system on a virtual disk with GPT layout and geometry close enough to the physical Windows disk for a fair VM comparison. Keep it separate from the existing `p3` Windows installation and never write the new installation to physical partitions.

The new installation must be activated and properly licensed before it is considered usable or complete. Verify that the existing Windows 11 Pro license permits using the VM installation while retaining the `p3` installation, and confirm Windows reports the VM as activated. Activation alone does not prove that the license covers both installations. If the existing license cannot cover the VM while preserving the existing installation and license state, stop and identify valid licensing options. Do not remove, overwrite, or invalidate the existing `p3` installation or its license state.

Compare the low-memory installation's boot time, responsiveness, and memory use with the existing Windows VM under the same KVM-backed configuration.
