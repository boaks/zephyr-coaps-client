![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# SH-CMDS 

```
help full
b 22.387 : > help:
b 22.387 :   at???        : generic modem at-cmd.(*)
b 22.387 :   band         : configure bands.(*?)
b 22.387 :   bat          : read battery status.
b 22.387 :   cfg          : configure modem.(*?)
b 22.387 :   con          : connect modem.(*?)
b 22.387 :   dev          : read device info.
b 22.387 :   edrx         : configure eDRX.(*?)
b 22.387 :   env          : read environment sensor.
b 22.387 :   eval         : evaluate connection.(*)
b 22.387 :   fota         : start application firmware-over-the-air update.(?)
b 22.387 :   imsi         : select IMSI.(*?)
b 22.387 :   interval     : send interval.(?)
b 22.387 :   led          : led command.(?)
b 22.388 :   limit        : read apn rate limit.(*)
b 22.388 :   mutex        : check mutex.
b 22.388 :   net          : read network info.(*)
b 22.388 :   off          : switch modem off.(*)
b 22.438 :   offline      : switch modem offline.(*)
b 22.438 :   on           : switch modem on.(*)
b 22.438 :   power        : configure power level.(*?)
b 22.438 :   psm          : configure PSM.(*?)
b 22.438 :   rai          : configure RAI.(*?)
b 22.438 :   reboot       : reboot device.
b 22.438 :   reboots      : read reboot codes.
b 22.438 :   remo         : reduced mobility.(*?)
b 22.438 :   reset        : modem factory reset.(*)
b 22.438 :   restart      : restart device.
b 22.438 :   restarts     : read restart reasons.
b 22.438 :   scan         : network scan.(*?)
b 22.438 :   search       : network search.(*)
b 22.438 :   send         : send message.(?)
b 22.439 :   sim          : read SIM-card info.(*)
b 22.439 :   sms          : send SMS.(*?)
b 22.489 :   state        : read modem state.(*)
b 22.489 :   storage      : list storage sections.
b 22.489 :   storageclear : clear all storage sections.
b 22.489 :   update       : start application firmware update. Requires XMODEM client.(?)
b 22.489 :   *            : AT-cmd is used, maybe busy.
b 22.489 :   ?            : help <cmd> available.
b 22.489 : 
b 22.539 : > help band:
b 22.539 :   band               : show current bands.
b 22.539 :   band all           : activate all bands.
b 22.539 :   band <b1> <b2> ... : activate bands <b1> <b2> ... .
b 22.539 : 
b 22.589 : > help cfg:
b 22.589 :   cfg         : read configuration.
b 22.589 :   cfg init    : reset configuration.
b 22.589 :   cfg <plmn> <modes>
b 22.589 :       <plmn>  : either auto or numerical plmn, e.g. 26202
b 22.589 :       <modes> : nb, m1, nb m1, m1 nb, or auto.
b 22.589 :               : nb    := NB-IoT
b 22.589 :               : m1    := LTE-M
b 22.589 :               : nb m1 := NB-IoT/LTE-M
b 22.589 :               : m1 nb := LTE-M/NB-IoT
b 22.589 :               : auto := LTE-M/NB-IoT without preference
b 22.589 : 
b 22.640 : > help con:
b 22.640 :   con         : read connection information
b 22.640 :   con <plmn> [<mode>]
b 22.640 :       <plmn>  : numerical plmn, e.g. 26202
b 22.640 :       <mode>  : optional mode, nb or m1.
b 22.640 :               : nb := NB-IoT
b 22.640 :               : m1 := LTE-M
b 22.640 :   con auto    : automatic network selection.
b 22.640 : 
b 22.690 : > help edrx:
b 22.690 :   edrx <edrx-time> : request eDRX time.
b 22.690 :      <edrx-time>   : eDRX time in s.
b 22.690 :                    : 0 to disable eDRX.
b 22.690 :                    : If modem is sleeping, this will be sent
b 22.690 :                    : to the network with the next connection.
b 22.690 :   edrx off         : disable eDRX.
b 22.690 :   edrx             : show current eDRX status.
b 22.690 : 
b 22.740 : > help fota:
b 22.740 :   fota apply <version>    : apply an already downloaded version.
b 22.740 :   fota download <version> : download a version.
b 22.740 :   fota update <version>   : download and apply a version.
b 22.740 :   fota cancel <version>   : cancel downloading a version.
b 22.740 : 
b 22.790 : > help imsi:
b 22.790 :   imsi           : show current IMSI selection.
b 22.790 :   imsi auto      : select IMSI automatically. Switching IMSI on timeout (300s).
b 22.790 :   imsi <n>       : select IMSI. Values 0 to 255.
b 22.790 :   imsi 0         : select IMSI automatically. Switching IMSI on timeout (300s).
b 22.790 :   imsi 1         : select IMSI 1. Fallback to latest successful IMSI.
b 22.790 :   imsi n         : select IMSI. The largest value depends on the SIM card
b 22.790 :   imsi force <n> : select IMSI. No fallback!
b 22.790 : 
b 22.841 : > help interval:
b 22.841 :   interval             : read send interval.
b 22.841 :   interval <time>[s|h] : set send interval.
b 22.841 :         <time>|<time>s : interval in seconds.
b 22.841 :                <time>h : interval in hours.
b 22.841 : 
b 23.891 : > help led:
b 23.891 :   led <color> <op> : apply opartion on color LED.
b 23.891 :       <color>      : red, blue, green, or all.
b 23.891 :               <op> : on, off, blink, or blinking.
b 23.891 : 
b 23.941 : > help power:
b 23.941 :   power     : show current power level.
b 23.941 :   power <l> : set power level. Values 0 to 4.
b 23.941 :         0   : Ultra-low power
b 23.941 :         1   : Low power
b 23.941 :         2   : Normal
b 23.941 :         3   : Performance
b 23.941 :         4   : High performance
b 23.941 : 
b 23.991 : > help psm:
b 23.991 :   psm <act-time> <tau-time>[h] : request PSM times.
b 23.991 :      <act-time>    : active time in s.
b 23.991 :      <tau-time>    : tracking area update time in s.
b 23.991 :      <tau-time>h   : tracking area update time in h.
b 23.991 :   psm normal       : PSM handled by application.
b 23.991 :   psm              : show current PSM status.
b 23.991 : 
b 23.042 : > help rai:
b 23.042 :   rai off|on : enable or disable RAI.
b 23.042 :   rai        : show current RAI status.
b 23.042 : 
b 23.092 : > help remo:
b 23.092 :   remo   : show current reduced mobility mode.
b 23.092 :   remo 0 : no reduced mobility.
b 23.092 :   remo 1 : reduced mobility (nordic).
b 23.092 :   remo 2 : no reduced mobility.
b 23.092 : 
b 23.142 : > help scan:
b 23.142 :   scan        : repeat previous network scan.
b 23.142 :   scan 0      : displays neighbor cell history
b 23.142 :   scan 1      : start neighbor cell search
b 23.142 :   scan 2      : start neighbor cell search, all bands
b 23.142 :   scan 3 <n>  : displays cell history
b 23.142 :   scan 4 <n>  : start cell search
b 23.142 :   scan 5 <n>  : start cell search, all bands
b 23.142 :   <n>         : maximum cells to list, values 2 to 15.
b 23.142 : 
b 23.192 : > help send:
b 23.192 :   send            : send application message.
b 23.192 :   send <message>  : send provided message.
b 23.192 : 
b 23.243 : > help sms:
b 23.243 :   sms                  : receive sms (120s).
b 23.243 :   sms <dest> <message> : send sms and receive sms (120s).
b 23.243 :   <dest>               : international IMSI
b 23.243 :   <message>            : message
b 23.243 : 
I 23.293 : > help update:
I 23.293 :   update          : start update download and reboot to apply it.
I 23.293 :   update download : start update download.
I 23.293 :   update info     : display current update info.
I 23.293 :   update erase    : erase current update.
I 23.293 :   update revert   : revert last update.
I 23.293 :   update reboot   : reboot to apply update.
```

** !!! Under Construction !!! **

