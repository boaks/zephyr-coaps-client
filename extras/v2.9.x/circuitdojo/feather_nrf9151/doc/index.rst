.. _circuitdojo_feather_nrf9151:

nRF9151 Feather
###############

.. figure:: img/circuitdojo_feather_nrf9151.jpg
     :align: center
     :alt: Circuit Dojo nRF9151 Feather

     nRF9151 Feather (Credit: Circuit Dojo)

Overview
********

The nRF9151 Feather by Circuit Dojo is a single-board development
for bringing your LTE-M and NB-IoT applications to life. The circuitdojo_feather_nrf9151
board configuration leverages the pre-existing support for the Nordic Semiconductor
nRF9151. Supported nRF9151 peripherals include:

* :abbr:`ADC (Analog to Digital Converter)`
* CLOCK
* FLASH
* :abbr:`GPIO (General Purpose Input Output)`
* :abbr:`I2C (Inter-Integrated Circuit)`
* :abbr:`MPU (Memory Protection Unit)`
* :abbr:`NVIC (Nested Vectored Interrupt Controller)`
* :abbr:`PWM (Pulse Width Modulation)`
* :abbr:`RTC (nRF RTC System Clock)`
* Segger RTT (RTT Console)
* :abbr:`SPI (Serial Peripheral Interface)`
* :abbr:`UARTE (Universal asynchronous receiver-transmitter with EasyDMA)`
* :abbr:`WDT (Watchdog Timer)`
* :abbr:`IDAU (Implementation Defined Attribution Unit)`

More information about the board can be found at the
`nRF9151 Feather Documentation`_.


Hardware
********

.. figure:: img/nrf9151-feather-v31-features.jpg
   :align: center
   :alt: nRF9151 Feather Features

Connections and IOs
===================

The nRF9151 Feather has everything you know and love about
the Feather platform. Here are some of the highlights:

LED
---

* D7 (blue) = P0.03

Push buttons and Switches
-------------------------

* MODE = P0.12
* RESET

USB
---

Contains a USB/UART connection for both debugging and loading new
code using a UART Enabled MCUBoot.

Standard Battery Connection
----------------------------

The nRF9151 Feather has a 2 pin battery connector on board. Lithium Polymer batteries >
300mA required.

Nano SIM Holder
---------------

The nRF9151 Feather has a built-in nano SIM (4FF) holder located
on the bottom side.


Programming and Debugging
*************************

circuitdojo_feather_nrf9151 has a Tag Connect TC2030-CTX-NL. It can be used
by most programmers like:

* J-Link (the nRF53-DK is recommended)
* CMSIS-DAP based programmers

Check out `Getting Started`_ for more info.

Building an application
=======================

In most cases you'll want to use the ``ns`` target with any of the Zephyr
or Nordic based examples.

.. note::
   Trusted Firmware-M (TF-M) and building the ``ns`` target is not supported for this board.

Some of the examples do not use secure mode, so they do not require the
``ns`` suffix. A great example of this is the `hello_world` below.

Flashing
========

Follow the instructions in the :ref:`nordic_segger` page to install
and configure all the necessary software. Further information can be
found in :ref:`nordic_segger_flashing`. Then build and flash
applications as usual (see :ref:`build_an_application` and
:ref:`application_run` for more details).

Here is an example for the :ref:`hello_world` application.

First, run your favorite terminal program to listen for output.

.. code-block:: console

   $ screen /dev/<tty_device> 115200

Replace :code:`<tty_device>` with the port where the nRF9151 Feather
can be found. In most cases (On Linux/Mac) it will be: :code:`/dev/tty.SLAB_USBtoUART`.

Then build and flash the application in the usual way.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: circuitdojo_feather_nrf9151
   :goals: build flash

Debugging
=========

Refer to the :ref:`nordic_segger` page to learn about debugging Nordic boards with a
Segger IC.


Testing the LEDs and buttons on the nRF9151 Feather
***************************************************

There are 2 samples that allow you to test that the buttons (switches) and LEDs on
the board are working properly with Zephyr:

* :ref:`blinky-sample`
* :ref:`button-sample`

You can build and flash the examples to make sure Zephyr is running correctly on
your board. The button and LED definitions can be found in
:zephyr_file:`boards/arm/circuitdojo_feather_nrf9151/circuitdojo_feather_nrf9151_common.dtsi`.

References
**********

.. target-notes::

**Side note** This page was based on the documentation for the nRF9151 DK. Thanks to Nordic for
developing a great platform!

.. _nRF9151 Feather Documentation: https://docs.circuitdojo.com/nrf9151-introduction.html
.. _Getting Started: https://docs.circuitdojo.com/nrf9151-getting-started.html
