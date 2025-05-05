![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# Ubuntu permission rules for some devices

In order to grant users the permission to use the device, copy the `rules` file into `/etc/udev/rules.d`. That requires super-user rights, so this usually is done with:

```
sudo cp 50* /etc/udev/rules.d
sudo udevadm control --reload-rules
```

Please unplug and plug the device again.

## nRF-USB - USB connector for most Nordic devices

[nrf-udev_1.0.1-all.deb](https://github.com/NordicSemiconductor/nrf-udev/releases)

Download the `.deb` and install it with

```
sudo apt install ./nrf-udev_1.0.1-all.deb
```

## CP210x - USB connector for nRF9160 feather & Conexio Stratus Pro

[50-cp210x.rules](./50-cp210x.rules) 

## CMSIS-DAP - USB connector for makediary nRF9151 Connect Kit 

[50-nordic-probe.rules](./50-nordic-probe.rules) 

## CMSIS-DAP - USB Raspberry Pi Debug Probe (nRF9161/51 feather)

[50-raspi-probe.rules](./50-raspi-probe.rules)

