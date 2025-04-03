![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with Eclipse/TinyDtls

# Ubuntu permission rules for some devices

In order to grant users the permission to use the device, copy the `rules` file into `/etc/udev/rules.d`. That requires super-user rights, so this usually is done with:

```
sudo cp 50* /etc/udev/rules.d
sudo udevadm control --reload-rules
```

Please unplug and plug the device again.

## CP210x - USB connector for nRF9160 feather 

[50-cp210x.rules](./50-cp210x.rules) 

## CMSIS-DAP - USB connector for makediary nRF9151 Connect Kit 

[50-nordic-probe.rules](./50-nordic-probe.rules) 

## CMSIS-DAP - USB Raspberry Pi Debug Probe (nRF9161/51 feather)

[50-raspi-probe.rules](./50-raspi-probe.rules)

## Use CMSIS-DAP with pyocd

To use CMSIS-DAP to flash a device, please adjust first the permissions by copying the rules files as described above.

To flash the application, you may either use `west` and `pyocd` as runner or `pyocd` directly.

```
west flash --runner pyocd
```

or
 
```
# application
pyocd load -t nrf91 <application.hex>
# modem firmware
pyocd cmd -t nrf91 -c 'nrf91-update-modem-fw -f mfw_nrf91x1_2.0.2.zip'# application
```

