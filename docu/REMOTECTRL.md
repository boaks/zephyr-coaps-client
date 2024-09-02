![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# Remote Control

Control commands have been introduced for the [Cellular Explorer](./CELLULAREXPLORER.md) and enabled a local connected user to execute several commands to explore the network and the neighbourhood. Over the time a couple of commands have been added to configure the device in the field. And with v0.9.0 it is also possible to send such command from the server back to the device in the response of the device request with the data.

Not all commands will make sense to be send by the server to the device, especially when the result is mainly an informative logging output as for the command "scan". Also some commands are dangerous, because they may brick the device. Changing the APN may cause a device to go offline, if the new APN doesn't work.

Therefore it is recommended to test all commands first locally via UART USB or Bluetooth LE and only if you are sure, that it works as expected, then you may send it back to the device. These first local tests should also include the help for the commands using `help <cmd>`.  

How the payload for the response is defined depends on the server application you're using. With the  [cf-s3-proxy-server](https://github.com/eclipse-californium/californium/tree/main/demo-apps/cf-s3-proxy-server) the main purpose of the device request is to store the data in S3. If the device is intended to get back some payload, then the device must add the query-parameter

```
?read
```

to the destination URI. The defaults to read the file "s3://${domain}/devices/cali.${imei}/config". If you want to read a different sub-resource of "s3://${domain}/devices/cali.${imei}/", then provide that as parameter, e.g.

```
?read=ctrl
```

The device is processing the payload line by line, if the content-type is "text/plain". Other content-types are not supported "out-of-box" but may be added as customization on your side.

Each of that line is the handled as "control command", if the line starts with "cmd", e.g.

```
cmd interval 1200
```

executed the command "interval 1200". That sets the send interval to 1200s. 

As mentioned above, quite a lot of the commands are developed to be applied locally. That is sometimes in conflict with the processing of the current response and sending the device into sleep mode. Therefore it is possible to add an delay for the execution in milliseconds.

```
cmd 5000 led blue blink
```

This will wait 5s (5000ms) before the blue LED blinks. If not delayed, the control of the LED is on conflict with the LED signaling for the current request.

## Remote commands

As for now, two commands are developed for being mainly applied remotely. One is "fota" and the other one is "rscan". 

### fota

For a remote update via FOTA, use "fota" command

```
cmd fota update 0.10.0
```

This will dowload the firmware "v0.10.0" and applies that with rebooting after downloading. During the download the device will report the status from time to time. As mentioned above, if you want to get some help, execute the help command locally.

```
help fota
I 00.123 : > help fota:
I 00.123 :   fota apply <version>    : apply an already downloaded version.
I 00.123 :   fota download <version> : download a version.
I 00.123 :   fota update <version>   : download and apply a version.
I 00.123 :   fota cancel <version>   : cancel downloading a version.
```

It is possible to split the download and the reboot to apply, if the timepoint of that reboot is critical. Using "cancel" you also have a chance to chancel the update. If that works, depends a lot on the status. Once the reboot started, it's too late.

### rscan

A rscan scans the network neighbourhood and reports the result to the server rather then only writing it to the log.

```
cmd rscan 5 6
```

Executes a network scan and report the result to the server.

```
CAT-M1 neighbor cell measurements 0/8
  # :  plmn    tac      cell  band earfnc pid rsrp/q dB(m)
[ 0]: 26203  0xe936 0x01656201  20  6200  251   -98/ -9
[ 1]: 26201  0x67b9 0x01CC2B06   3  1444  270  -106/ -5
[ 2]: 26201  0x67b9 0x01CC2B03  20  6400  206  -101/-11
[ 3]: 26201  0x67b9 0x01CC2B05  20  6400  205  -101/-11
[*4]: 26201  0x67b9 0x01CC2B00   3  1300   78  -106/-10
[ 5]: 26202  0xb982 0x031D7801  20  6300  197  -106/-13
[ 6]: 26203  0xe936 0x016E4017  20  6200  380  -108/-18
[ 7]: 26201  0x67b9 0x022A1111  20  6400   28  -108/-18
(*4 : current cell)
Scans 1, improves 0, 69 s, overall 0 s
```

The list contains the `plmn`, `tac` and `cell`, along with the `band`, the frequency (`earfnc`), `pid` and the radio signal condition (`rsrp` and `rsrq`). The current cell is marked with an `*`. Please note, that the result is changing over the time pretty fast. Sometimes even the current cell is not contained in the list.

To see the options, also use "help rscan" locally.

```
help rscan
I 12.472 : > help rscan: remote network scan
I 12.472 :   rscan        : repeat previous network scan.
I 12.472 :   rscan 0      : displays neighbor cell history
I 12.472 :   rscan 1      : start neighbor cell search
I 12.472 :   rscan 2      : start neighbor cell search, all bands
I 12.472 :   rscan 3 <n>  : displays cell history
I 12.472 :   rscan 4 <n>  : start cell search
I 12.472 :   rscan 5 <n>  : start cell search, all bands
I 12.472 :   <n>         : maximum cells to list, values 2 to 15.
```

** !!! Under Construction !!! **
