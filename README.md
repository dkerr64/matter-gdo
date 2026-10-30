# Matter Garage Door Closure

This example creates a Matter Closure device for a garage door using the [ESP-Matter SDK](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/) from Espressif.

> [!NOTE]
> Current Smart Home platforms do not yet support garage doors through with Matter.  See [Smart Home Compatibility](#smart-home-compatibility) below.

## Prerequisites

- ESP-IDF version [v5.5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32/index.html)
- GDOLIB version [TBD](https://github.com/dkerr64/gdolib)

## Creating a VS Code based build system

Install required packages on Linux...

```bash
sudo apt install -y build-essential git cmake curl wget usbutils ccache
sudo apt install -y python3-pip python3-venv python-is-python3 python3-serial
sudo apt install -y libncurses-dev flex bison dfu-util libffi-dev libusb-1.0-0 libssl-dev ripgrep gperf
```

### Development environment setup

The following assumes VS Code as the development environment with ESP-IDF extension.  Make sure to install the ESP-IDF version documented in prerequisites above, do not assume that this will work with the latest version.  Other recommended extensions include Microsoft's C/C++ Intellisense, and Remote SSH Editing if you are using a VM or Container build system.

After you have VS Code and ESP-IDF extension installed...

```bash
# Make sure locales are generated
sudo locale-gen en_US.UTF-8
sudo dpkg-reconfigure locales
# Set Github name, email, and setup to save our credentials
git config --global --edit
git config --global credential.helper store
# Clone the repo... will prompt for credentials
git clone --recurse-submodules https://github.com/dkerr64/matter-gdo.git
cd matter-gdo
git submodule update --init --recursive
```

```bash
cd matter-gdo
cp sdkconfig.defaults.ratgdo32 sdkconfig.defaults
cp CMakePresets.json.ratgdo32 CMakePresets.json
cp .vscode/c_cpp_properties.json.ratgdo32 .vscode/c_cpp_properties.json
cp .vscode/settings.json.ratgdo32 .vscode/settings.json
```

### Build Configuration

This repository includes _sdkconfig.defaults.xxx_ files for the [Ratgdo32](https://ratcloud.llc/) and [GRGDOv3](https://www.gelidus.ca/grgdov3-setup-guide/) boards. Copy one of those to _sdkconfig.defaults_ and when you run SDK configuration or menuconfig you can set the following...

**CONFIG_GDO_xxxx_PIN** set the GPIO pin numbers for the specific board.  The provided Ratgdo32 and GRGDOv3 defaults files include the correct settings.

**CONFIG_GDO_ENABLE_SYSLOG**, If enabled then, in addition to logging to the USB serial port, log messages will also be sent to a syslog server.  The syslog server is discovered using mDNS searching for _\_syslog.\_udp_ and the IPv4 and port number saved across reboots.  If the syslog server changes then you must reboot the device for it to discover the new IPv4/port.  If a syslog server is not found within 10 minutes of the device connecting to a WiFi network then it will stop searching. (default off).

**CONFIG_ENABLE_CHIP_SHELL**, If enabled then the standard ESP-Matter USB serial port console will be available, including additional commands unique to this device.  Type _gdo_ into the console for list of our commands, or _matter help_ for Matter-SDK provided commands. (default off).

Paths inside the VS Code _c\_cpp\_properties.json_ and _settings.json_ files will need to be updated for your environment, the steps listed in first build should do this for you...

### First Build

In VS Code open the folder that you cloned matter-gdo into, then in the ESP-IDF Explorer...

1. Select Project Workspace to this folder
2. Select ESP-IDF version (see above prerequisites)
3. Select Flash method, UART is most likely to work
4. Select Port to Use (there is a default in _settings.json_ but change to match your environment)
5. Select Monitor Port to Use (there is a default in _settings.json_ but change to match your environment)
6. Select Project Configuration (use default)
7. Set Espressif Device Target (eg. esp32 or esp32c6)
8. SDK Configuration Editor (or classic menuconfig under Advanced), save the configuration.

You can now select _Build Project_ in ESP-IDF Explorer, there are well over a thousand source files to compile so it will take some time.

> [!NOTE]
> The build will use all available CPU cores, so if you are developing in a Virtual Machine or Container environment then its a good idea to configure it with as many virtual cores as your host system will accommodate... perhaps a couple less than the hardware CPU supports to provide some headroom for the host.

Once the build completes you can flash the firmware to the device.

## Adding the GDO device to your Smart Home

> [!IMPORTANT]
>The Matter Specifications include security features designed to protect the overall integrity of a Smart Home environment, all commercial Matter-based products must be certified by the Connectivity Standards Alliance (CSA).  Certification requires that the manufacturer be a member of the CSA, and submit their products for testing.
>
>The _matter-gdo_ project is development/test and has not been submitted for certification, the credentials used when adding the device to a Smart Home network are based on the test/dev certificates and use _TEST\_VENDOR_ and _TEST\_PRODUCT_ identifiers provided by the CSA for this purpose.
>
>You must flash the provided _factory\_partition.bin_ file to the device before you can add it to your Smart Home, described below.

### Commissioning to Matter fabric

After building the firmware and flashing to the ESP device for the first time, you must also flash the commissioning credentials to the factory partition on the ESP board before you can add it to your Smart Home

> [!NOTE]
> All commands must be executed in a terminal activated for the ESP-IDF development environment.  Selecting _Open ESP-IDF Terminal_ from VS Code status bar does this automatically

```bash
# Set ESPTOOL_PORT to match your environment, or use the -p/--port option on esptool.py command line.
export ESPTOOL_PORT=/dev/tty.usbmodem1101
esptool.py write_flash 0x3e0000 factory_partition.bin
```

Once the flash completes and the board restarts you can add it to your Smart Home using the QR code below or a manual set-up of **1508-871-5328**.  The QR code is generated from the payload **MT:634J0CEK01UC7G06K00**
![MT:634J0CEK01UC7G06K00](proj_qrcode.png)

### Using your own credentials

You can use your own credentials if you wish.  If you are a member of the CSA and have your own Vendor ID and registered Product ID you should use those, and if manufacturing multiple boards, flash unique credentials into each one during the manufacturing process.  See the ESP-Matter SDK and CSA documentation.

For our dev/test purposes we used the _Chip-Test-CD-FFF2-8001.der_ Certificate Delegation (CD) file provided in the [connectedhomeip credentials](https://github.com/project-chip/connectedhomeip/tree/master/credentials) folder and create the flash binary.  A copy of that DER file is included in this repository so you can use these commands...

```bash
pip install esp-matter-mfg-tool

esp-matter-mfg-tool --passcode 25102023 \
                    --discriminator 1809 \
                    -cd Chip-Test-CD-FFF2-8001.der  \
                    -v 0xfff2 \
                    -p 0x8001 \
                    --hw-ver 1 \
                    --hw-ver-str DevKit \
                    --outdir build \
                    --target esp32c6
```

If you need to create your own CD file then you can use the `chip-cert` tool provided by the CSA.

## Smart Home Compatibility

Garage Door support was added to the CSA Matter Specifications in version 1.5.  Current Smart Home environments are not yet updated to 1.5 and do not support Garage Doors through the Matter protocol.  Current status at time of writing...

**Apple Home**, Successfully adds the device but exposes only the light and lock (remotes) endpoints.  There is no garage door endpoint available.

**Samsung Smartthings**, Successfully added the device but exposes only the lock (remotes) endpoint.  There is no light or garage door endpoint available.

**IKEA Home Smart**, Did not pair using QR Code.  Successfully paired entering the code manually but exposes only the light endpoint.  There is no lock (remotes) or garage door endpoint available.

**Matter Utilities**, for [iOS and MacOS](https://matter-utilities.com/), Successfully added the device and registers all endpoints.  However can only control the light.

**Home Assistant**, Home Assistant currently requires that you enable support for Test-net DCL in order to pair a non-CSA certified device.  Find it in Settings->Apps->Matter Server->Configuration.  Once paired, only the light and lock (remotes) endpoints are available. Support for Matter closures, including for garage doors, is under development and can be tested using [PR 183177](https://github.com/home-assistant/core/pull/183177).

**Google Home**, Not tested.

**Amazon Alexa**, Not tested.

**CSA chip-tool**, Successfully added and can control device, see below.

## Testing with CHIP Tool

The Connectivity Standards Alliance (CSA) publishes [chip-tool](https://github.com/project-chip/connectedhomeip/tree/master/examples/chip-tool) for testing Matter devices, it is available on GitHub within the examples directory of the _connectedhomeip_ repository. You will have to build this from source, it is not part of this project, nor included in the ESP-Matter SDK from Espressif.

You need to run chip-tool on a system that has Bluetooth Low Energy (BLE) and WiFi.  If using an Apple MacBook then note the requirement to install a bluetooth developer profile, see documentation [here](https://github.com/project-chip/connectedhomeip/blob/master/docs/guides/darwin.md).

Pair this device with your test system

```bash
chip-tool pairing ble-wifi 1 <WiFiSSID> <WiFiPassword> 25102023 1809
# 1 == user assigned node-id (I picked 1 for simplicity)
# 25102023 == this device default setup pin
# 1809 == this device default discriminator
```

Once paired, you can control the device.  Note that in these examples we provide both the node-id and an endpoint-id.  Currently endpoint-id's are:

0. Root node for device information
1. ModeSelect (for custom device control)
2. ClosureControl (for GDO positioning)
3. ClosureDimension (for GDO information and positioning)
4. OnOff (for GDO light)
5. DoorLock (for GDO remotes enable/disable)
6. OccupancySensor (for motion)

```bash
chip-tool descriptor read parts-list 1 0
# Retrieves all endpoints from device

chip-tool descriptor read server-list 1 4
# Find out what each endpoint can do
# 1 == node-id set during pairing
# 4 = endpoint-id for the light

chip-tool descriptor read device-type-list 1 3
# Find out what type each endpoint is
# 1 == node-id set during pairing
# 3 = endpoint-id for the closure dimension

chip-tool onoff toggle 1 4
# 1 == node-id set during pairing
# 4 = endpoint-id for the light

chip-tool closurecontrol move-to 1 2 --Position 1 --timedInteractionTimeoutMs 1000
# Position 1 is fully open, position 0 is fully closed.

chip-tool closuredimension set-target 1 3 --Position 10000 --timedInteractionTimeoutMs 1000
# Target position is 0 to 100 percent in 100ths (0 is fully open, 10000 is fully closed)

chip-tool modeselect read current-mode 1 1
# Will report back 0.  Two modes are supported, 0 (normal) and 1 (reboot).
# The device resets to mode 0 on every reboot

chip-tool modeselect change-to-mode 1 1 1
# Requesting a change to mode 1 instructs the device to reboot (after 1 second delay)
```

You can also use chip-tool in interactive mode and then can omit typing `chip-tool` in front of each command...

```bash
chip-tool interactive start
```
