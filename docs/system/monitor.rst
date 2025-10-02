.. _QEMU monitor:

QEMU Monitor
------------

The QEMU monitor is used to give complex commands to the QEMU emulator.
You can use it to:

-  Remove or insert removable media images (such as CD-ROM or
   floppies).

-  Freeze/unfreeze the Virtual Machine (VM) and save or restore its
   state from a disk file.

-  Inspect the VM state without an external debugger.

-  Enable or disable debug logging for various components.

Commands
~~~~~~~~

The following commands are available:

.. hxtool-doc:: hmp-commands.hx

.. hxtool-doc:: hmp-commands-info.hx

Debug Commands
~~~~~~~~~~~~~

.. cmdoption:: cpu-stats-debug [on|off]

   Enable or disable debug logging for CPU statistics. When enabled, detailed information about CPU operations and performance metrics will be printed.

.. cmdoption:: sparc-cpu-debug [on|off]

   Enable or disable debug logging for SPARC CPU operations. When enabled, detailed information about SPARC-specific CPU operations and state changes will be printed.

Integer expressions
~~~~~~~~~~~~~~~~~~~

The monitor understands integers expressions for every integer argument.
You can use register names to get the value of specifics CPU registers
by prefixing them with *$*.
