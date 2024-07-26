![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# SH-CMDS 

```
help full
b 93.861 : > help: (v0.10.102+2)
b 93.861 :   at???        : generic modem at-cmd.(*)
b 93.861 :   apn          : modem APN.(*?)
b 93.861 :   apnclr       : clear modem APN.(*)
b 93.861 :   ban          : set PLMNs to forbidden list (SIM-card).(*?)
b 93.861 :   banclr       : clear forbidden PLMN list (SIM-card).(*)
b 93.861 :   band         : configure bands.(*?)
b 93.861 :   bat          : read battery status.
b 93.862 :   cfg          : configure modem.(*?)
b 93.862 :   con          : connect modem.(*?)
b 93.862 :   deep         : network deep-search mode.(*?)
b 93.862 :   del          : delete settings.(#?)
b 93.862 :   dest         : show destination.
b 93.862 :   dev          : read device info.
b 93.862 :   dtls         : show dtls information.(?)
b 93.862 :   edrx         : configure eDRX.(*?)
b 93.862 :   env          : read environment sensor.
b 93.862 :   eval         : evaluate connection.(*)
b 93.912 :   fota         : start application firmware-over-the-air update.(?)
b 93.912 :   genec        : generate ec keypair.(#)
b 93.912 :   genpsk       : generate psk secret.(#)
b 93.912 :   get          : get settings.(?)
b 93.912 :   iccid        : read ICCID.(*)
b 93.912 :   imsi         : select IMSI.(*?)
b 93.912 :   interval     : send interval.(?)
b 93.913 :   led          : led command.(?)
b 93.913 :   limit        : read apn rate limit.(*)
b 93.913 :   lock         : lock protected cmds.
b 93.913 :   net          : read network info.(*)
b 93.913 :   off          : switch modem off.(*)
b 93.913 :   offline      : switch modem offline.(*)
b 93.913 :   on           : switch modem on.(*)
b 93.913 :   power        : configure power level.(*?)
b 93.913 :   prio         : set PLMNs to user HP list (SIM-card).(*?)
b 93.963 :   prioclr      : clear user HPPLMN list (SIM-card).(*)
b 93.963 :   prov         : show provisioning data.
b 93.963 :   provdone     : provisioning done.(#)
b 93.963 :   psm          : configure PSM.(*?)
b 93.963 :   rai          : configure RAI.(*?)
b 93.963 :   reboot       : reboot device.(?)
b 93.963 :   reboots      : read reboot codes.
b 93.963 :   remo         : reduced mobility.(*?)
b 93.964 :   reset        : modem factory reset.(*)
b 93.964 :   restart      : try to switch off the modem and restart device.
b 93.964 :   restarts     : read restart reasons.
b 93.964 :   rscan        : remote network scan.(*?)
b 93.964 :   scan         : network scan.(*?)
b 93.964 :   search       : network search.(*)
b 93.964 :   send         : send message.(?)
b 93.964 :   sendflags    : sendflags.(?)
b 93.014 :   sendresult   : send result message.
b 93.014 :   set          : set settings from text.(#?)
b 93.014 :   sethex       : set settings from hexadezimal.(#?)
b 93.014 :   sim          : read SIM-card info.(*)
b 93.014 :   sms          : send SMS.(*?)
b 93.014 :   state        : read modem state.(*)
b 93.014 :   storage      : list storage sections.
b 93.014 :   storageclear : clear all storage sections.
b 93.014 :   time         : show system time.
b 93.015 :   timeout      : initial coap timeout.(?)
b 93.015 :   unlock       : unlock protected cmds.(?)
b 93.015 :   update       : start application firmware update. Requires XMODEM client.(?)
b 93.015 :   *            : AT-cmd is used, maybe busy.
b 93.015 :   #            : protected <cmd>, requires 'unlock' ahead.
b 93.015 :   ?            : help <cmd> available.
b 93.015 : 
b 93.065 : > help apn:
b 93.065 :   apn <apn>  : set and active APN.
b 93.065 :   apn        : show current APN.
b 93.065 : 
b 93.115 : > help ban:
b 93.115 :   ban                      : show ban-list.
b 93.115 :   ban <plmn> [<plmn2> ...] : set plmn(s) as ban-list.
b 93.115 : 
b 93.165 : > help band:
b 93.165 :   band               : show current bands.
b 93.165 :   band all           : activate all bands.
b 93.165 :   band <b1> <b2> ... : activate bands <b1> <b2> ... .
b 93.165 : 
b 93.215 : > help cfg:
b 93.215 :   cfg         : read configuration.
b 93.215 :   cfg init    : reset configuration.
b 93.215 :   cfg <plmn> <modes>
b 93.215 :       <plmn>  : either auto or numerical plmn, e.g. 26202
b 93.215 :       <modes> : nb, m1, nb m1, m1 nb, or auto.
b 93.215 :               : nb    := NB-IoT
b 93.215 :               : m1    := LTE-M
b 93.215 :               : nb m1 := NB-IoT/LTE-M
b 93.215 :               : m1 nb := LTE-M/NB-IoT
b 93.215 :               : auto := LTE-M/NB-IoT without preference
b 93.215 : 
b 93.266 : > help con:
b 93.266 :   con         : read connection information
b 93.266 :   con <plmn> [<mode>]
b 93.266 :       <plmn>  : numerical plmn, e.g. 26202
b 93.266 :       <mode>  : optional mode, nb or m1.
b 93.266 :               : nb := NB-IoT
b 93.266 :               : m1 := LTE-M
b 93.266 :   con auto    : automatic network selection.
b 93.266 : 
b 94.316 : > help deep:
b 94.316 :   deep (on|1)  : enable deep-search.
b 94.316 :   deep (off|0) : disable deep-search.
b 94.316 :   deep         : show deep-search status.
b 94.316 : 
b 94.366 : > help del:
b 94.366 :   del <key>  : delete value for key.
b 94.366 : 
b 94.416 : > help dtls:
b 94.416 :   dtls       : show dtls details.
b 94.416 :   dtls reset : reset dtls session.
b 94.416 : 
b 94.466 : > help edrx:
b 94.466 :   edrx <edrx-time> : request eDRX time.
b 94.466 :      <edrx-time>   : eDRX time in s.
b 94.466 :                    : 0 to disable eDRX.
b 94.466 :                    : If modem is sleeping, the eDRX settings will be
b 94.466 :                    : sent to the network with the next connection.
b 94.466 :   edrx off         : disable eDRX.
b 94.466 :   edrx             : show current eDRX status.
b 94.466 : 
b 94.516 : > help fota:
b 94.516 :   fota apply <version>    : apply an already downloaded version.
b 94.516 :   fota download <version> : download a version.
b 94.517 :   fota update <version>   : download and apply a version.
b 94.517 :   fota cancel <version>   : cancel downloading a version.
b 94.517 : 
b 94.567 : > help get:
b 94.567 :   get <key>  : get value for key.
b 94.567 : 
b 94.617 : > help imsi:
b 94.617 :   imsi           : show current IMSI selection.
b 94.617 :   imsi auto      : select IMSI automatically. Switching IMSI on timeout (300s).
b 94.617 :   imsi next      : select next IMSI.
b 94.617 :   imsi <n>       : select IMSI. Values 0 to 10.
b 94.617 :   imsi 0         : select IMSI automatically. Switching IMSI on timeout (300s).
b 94.617 :   imsi 1         : select IMSI 1. Fallback to latest successful IMSI.
b 94.617 :   imsi n         : select IMSI. The largest value depends on the SIM card
b 94.617 :   imsi force <n> : select IMSI. No fallback!
b 94.617 : 
b 94.667 : > help interval:
b 94.667 :   interval               : read send interval.
b 94.667 :   interval <time>[s|m|h] : set send interval.
b 94.667 :         <time>|<time>s   : interval in seconds.
b 94.667 :                <time>m   : interval in minutes.
b 94.667 :                <time>h   : interval in hours.
b 94.667 : 
b 94.717 : > help led:
b 94.717 :   led <color> <op> : apply opartion on color LED.
b 94.717 :       <color>      : red, blue, green, or all.
b 94.717 :               <op> : on, off, blink, or blinking.
b 94.717 : 
b 94.767 : > help power:
b 94.767 :   power     : show current power level.
b 94.767 :   power <l> : set power level. Values 0 to 4.
b 94.767 :         0   : Ultra-low power
b 94.767 :         1   : Low power
b 94.767 :         2   : Normal
b 94.767 :         3   : Performance
b 94.768 :         4   : High performance
b 94.768 : 
b 94.818 : > help prio:
b 94.818 :   prio                      : show user HPPLMN list.
b 94.818 :   prio <plmn> [<plmn2> ...] : set plmn(s) as user HPPLMN list.
b 94.818 : 
b 94.868 : > help psm:
b 94.868 :   psm <act-time> <tau-time>[h] : request PSM times.
b 94.868 :      <act-time>    : active time in s.
b 94.868 :      <tau-time>    : tracking area update time in s.
b 94.868 :      <tau-time>h   : tracking area update time in h.
b 94.868 :   psm normal       : PSM handled by application.
b 94.868 :   psm              : show current PSM status.
b 94.868 : 
b 94.918 : > help rai:
b 94.918 :   rai off|on : enable or disable RAI.
b 94.918 :   rai        : show current RAI status.
b 94.918 : 
b 94.968 : > help reboot:
b 94.968 :   reboot     : reboot device <last> (forced).
b 94.968 :   reboot <n> : reboot device <n>, if <last> was not the same <n>.
b 94.968 : 
b 94.018 : > help remo:
b 94.018 :   remo   : show current reduced mobility mode.
b 94.018 :   remo 0 : no reduced mobility.
b 94.018 :   remo 1 : reduced mobility (nordic).
b 94.018 :   remo 2 : no reduced mobility.
b 94.019 : 
b 94.069 : > help rscan: remote network scan
b 94.069 :   rscan        : repeat previous network scan.
b 94.069 :   rscan 0      : displays neighbor cell history
b 94.069 :   rscan 1      : start neighbor cell search
b 94.069 :   rscan 2      : start neighbor cell search, all bands
b 94.069 :   rscan 3 <n>  : displays cell history
b 94.069 :   rscan 4 <n>  : start cell search
b 94.069 :   rscan 5 <n>  : start cell search, all bands
b 94.069 :   <n>         : maximum cells to list, values 2 to 15.
b 94.069 : 
b 94.119 : > help scan: start network scan
b 94.119 :   scan        : repeat previous network scan.
b 94.119 :   scan 0      : displays neighbor cell history
b 94.119 :   scan 1      : start neighbor cell search
b 94.119 :   scan 2      : start neighbor cell search, all bands
b 94.119 :   scan 3 <n>  : displays cell history
b 94.119 :   scan 4 <n>  : start cell search
b 94.119 :   scan 5 <n>  : start cell search, all bands
b 94.119 :   <n>         : maximum cells to list, values 2 to 15.
b 94.119 : 
b 94.169 : > help send:
b 94.169 :   send            : send application message.
b 94.169 :   send <message>  : send provided message.
b 94.169 : 
b 94.219 : > help sendflags:
b 94.219 :   sendflags                  : read coap sendflags.
b 94.220 :   sendflags <flags>          : set coap sendflags.
b 94.220 :             <flags>          : flags in decimal.
b 94.220 :             <0xflags>        : flags in hexadecimal.
b 94.220 :   sendflags <id> [<id2> ...] : set coap from names.
b 94.220 :             nores            : request without response (flag 1).
b 94.220 :             init             : initial infos (flag 2).
b 94.220 :             min              : minimal infos (flag 4).
b 94.220 :             dev              : device info (flag 16).
b 94.220 :             sim              : sim-card info (flag 32).
b 94.220 :             net              : network info (flag 64).
b 94.220 :             stat             : network statistics (flag 128).
b 94.220 :             env              : environment info (flag 512).
b 94.220 :             scan             : network scan result (flag 2048).
b 94.220 : 
b 94.270 : > help set:
b 94.270 :   set <key> <value>    : set value to key.
b 94.270 : 
b 95.320 : > help sethex:
b 95.320 :   set <key> <hex-value>    : set hexadecimal value for key.
b 95.320 : 
b 95.370 : > help sms:
b 95.370 :   sms                  : receive sms (120s).
b 95.370 :   sms <dest> <message> : send sms and receive sms (120s).
b 95.370 :   <dest>               : international IMSI
b 95.370 :   <message>            : message
b 95.370 : 
b 95.420 : > help timeout:
b 95.421 :   timeout        : read initial coap timeout.
b 95.421 :   timeout <time> : set initial coap timeout in seconds.
b 95.421 : 
I 95.471 : > help unlock:
I 95.471 :   unlock <password>  : unlock protected cmds for 60s.
I 95.471 : 
I 95.521 : > help update:
I 95.521 :   update          : start update download and reboot to apply it.
I 95.521 :   update download : start update download.
I 95.521 :   update info     : display current update info.
I 95.521 :   update erase    : erase current update.
I 95.521 :   update revert   : revert last update.
I 95.521 :   update reboot   : reboot to apply update.
```

Some commands my be disabled depending on the buidl, e.g. `update` (update via UART) will not be available for production images. Therefore check with `help`, which commands are available for your build.

** !!! Under Construction !!! **

