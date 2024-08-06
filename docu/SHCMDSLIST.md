![Zephyr logo](https://github.com/zephyrproject-rtos/zephyr/raw/main/doc/_static/images/kite.png)

## Zephyr - Coaps Demo Client with TinyDtls

** !!! Under Construction !!! **

# SH-CMDS 

```
help full
b 34.567 : > help: (v0.10.105+1)
b 34.567 :   at???        : generic modem at-cmd.(*)
b 34.567 :   apn          : modem APN.(*?)
b 34.567 :   apnclr       : clear modem APN.(*)
b 34.567 :   ban          : set PLMNs to forbidden list (SIM-card).(*?)
b 34.567 :   banclr       : clear forbidden PLMN list (SIM-card).(*)
b 34.567 :   band         : configure bands.(*?)
b 34.567 :   bat          : read battery status.
b 34.567 :   cfg          : configure modem.(*?)
b 34.567 :   con          : connect modem.(*?)
b 34.567 :   deep         : network deep-search mode.(*?)
b 34.567 :   del          : delete settings.(#?)
b 34.567 :   dest         : show destination.
b 34.567 :   dev          : read device info.
b 34.567 :   dtls         : show dtls information.(?)
b 34.567 :   edrx         : configure eDRX.(*?)
b 34.567 :   env          : read environment sensor.
b 34.567 :   eval         : evaluate connection.(*)
b 34.618 :   fota         : start application firmware-over-the-air update.(?)
b 34.618 :   genec        : generate ec keypair.(#)
b 34.618 :   genpsk       : generate psk secret.(#)
b 34.618 :   get          : get settings.(?)
b 34.618 :   iccid        : read ICCID.(*)
b 34.618 :   imsi         : select IMSI.(*?)
b 34.618 :   interval     : send interval.(?)
b 34.618 :   led          : led command.(?)
b 34.618 :   limit        : read apn rate limit.(*)
b 34.618 :   lock         : lock protected cmds.
b 34.618 :   net          : read network info.(*)
b 34.618 :   off          : switch modem off.(*)
b 34.618 :   offline      : switch modem offline.(*)
b 34.618 :   on           : switch modem on.(*)
b 34.618 :   power        : configure power level.(*?)
b 34.618 :   prio         : set PLMNs to user HP list (SIM-card).(*?)
b 34.669 :   prioclr      : clear user HPPLMN list (SIM-card).(*)
b 34.669 :   prov         : show provisioning data.
b 34.669 :   provdone     : provisioning done.(#)
b 34.669 :   psm          : configure PSM.(*?)
b 34.669 :   rai          : configure RAI.(*?)
b 34.669 :   reboot       : reboot device.(?)
b 34.669 :   reboots      : read reboot codes.
b 34.669 :   remo         : reduced mobility.(*?)
b 34.669 :   reset        : modem factory reset.(*)
b 34.669 :   restart      : try to switch off the modem and restart device.
b 34.669 :   restarts     : read restart reasons.
b 34.669 :   rscan        : remote network scan.(*?)
b 34.669 :   scan         : network scan.(*?)
b 34.669 :   search       : network search.(*)
b 34.669 :   send         : send message.(?)
b 34.669 :   sendflags    : sendflags.(?)
b 34.720 :   sendresult   : send result message.
b 34.720 :   set          : set settings from text.(#?)
b 34.720 :   sethex       : set settings from hexadezimal.(#?)
b 34.720 :   sim          : read SIM-card info.(*)
b 34.720 :   sms          : send SMS.(*?)
b 34.720 :   state        : read modem state.(*)
b 34.720 :   storage      : list storage sections.
b 34.720 :   storageclear : clear all storage sections.
b 34.720 :   time         : show system time.
b 34.720 :   timeout      : initial coap timeout.(?)
b 34.720 :   unlock       : unlock protected cmds.(?)
b 34.720 :   update       : start application firmware update. Requires XMODEM client.(?)
b 34.720 :   *            : AT-cmd is used, maybe busy.
b 34.720 :   #            : protected <cmd>, requires 'unlock' ahead.
b 34.720 :   ?            : help <cmd> available.
b 34.720 : 
b 34.770 : > help apn:
b 34.770 :   apn <apn>  : set and active APN.
b 34.770 :   apn        : show current APN.
b 34.770 : 
b 34.820 : > help ban:
b 34.820 :   ban                      : show ban-list.
b 34.820 :   ban <plmn> [<plmn2> ...] : set plmn(s) as ban-list.
b 34.821 : 
b 34.871 : > help band:
b 34.871 :   band               : show current bands.
b 34.871 :   band all           : activate all bands.
b 34.871 :   band <b1> <b2> ... : activate bands <b1> <b2> ... .
b 34.871 : 
b 34.921 : > help cfg:
b 34.921 :   cfg         : read configuration.
b 34.921 :   cfg init    : reset configuration.
b 34.921 :   cfg <plmn> <modes>
b 34.921 :       <plmn>  : either auto or numerical plmn, e.g. 26202
b 34.921 :       <modes> : nb, m1, nb m1, m1 nb, or auto.
b 34.921 :               : nb    := NB-IoT
b 34.921 :               : m1    := LTE-M
b 34.921 :               : nb m1 := NB-IoT/LTE-M
b 34.921 :               : m1 nb := LTE-M/NB-IoT
b 34.921 :               : auto := LTE-M/NB-IoT without preference
b 34.921 : 
b 34.971 : > help con:
b 34.971 :   con         : read connection information
b 34.971 :   con <plmn> [<mode>]
b 34.971 :       <plmn>  : numerical plmn, e.g. 26202
b 34.971 :       <mode>  : optional mode, nb or m1.
b 34.971 :               : nb := NB-IoT
b 34.971 :               : m1 := LTE-M
b 34.971 :   con auto    : automatic network selection.
b 34.971 : 
b 34.021 : > help deep:
b 34.021 :   deep (on|1)  : enable deep-search.
b 34.021 :   deep (off|0) : disable deep-search.
b 34.021 :   deep         : show deep-search status.
b 34.021 : 
b 35.071 : > help del:
b 35.071 :   del <key>  : delete value for key.
b 35.071 : 
b 35.122 : > help dtls:
b 35.122 :   dtls       : show dtls details.
b 35.122 :   dtls reset : reset dtls session.
b 35.122 : 
b 35.172 : > help edrx:
b 35.172 :   edrx <edrx-time> : request eDRX time.
b 35.172 :      <edrx-time>   : eDRX time in s.
b 35.172 :                    : 0 to disable eDRX.
b 35.172 :                    : If modem is sleeping, the eDRX settings will be
b 35.172 :                    : sent to the network with the next connection.
b 35.172 :   edrx off         : disable eDRX.
b 35.172 :   edrx             : show current eDRX status.
b 35.172 : 
b 35.222 : > help fota:
b 35.222 :   fota apply <version>    : apply an already downloaded version.
b 35.222 :   fota download <version> : download a version.
b 35.222 :   fota update <version>   : download and apply a version.
b 35.222 :   fota cancel <version>   : cancel downloading a version.
b 35.222 : 
b 35.272 : > help get:
b 35.272 :   get <key>  : get value for key.
b 35.272 : 
b 35.322 : > help imsi:
b 35.322 :   imsi           : show current IMSI selection.
b 35.322 :   imsi auto      : select IMSI automatically. Switching IMSI on timeout (300s).
b 35.322 :   imsi next      : select next IMSI.
b 35.322 :   imsi <n>       : select IMSI. Values 0 to 10.
b 35.322 :   imsi 0         : select IMSI automatically. Switching IMSI on timeout (300s).
b 35.322 :   imsi 1         : select IMSI 1. Fallback to latest successful IMSI.
b 35.322 :   imsi n         : select IMSI. The largest value depends on the SIM card
b 35.322 :   imsi force <n> : select IMSI. No fallback!
b 35.322 : 
b 35.372 : > help interval:
b 35.372 :   interval               : read send interval.
b 35.372 :   interval <time>[s|m|h] : set send interval.
b 35.373 :         <time>|<time>s   : interval in seconds.
b 35.373 :                <time>m   : interval in minutes.
b 35.373 :                <time>h   : interval in hours.
b 35.373 : 
b 35.423 : > help led:
b 35.423 :   led <color> <op> : apply opartion on color LED.
b 35.423 :       <color>      : red, blue, green, or all.
b 35.423 :               <op> : on, off, blink, or blinking.
b 35.423 : 
b 35.473 : > help power:
b 35.473 :   power     : show current power level.
b 35.473 :   power <l> : set power level. Values 0 to 4.
b 35.473 :         0   : Ultra-low power
b 35.473 :         1   : Low power
b 35.473 :         2   : Normal
b 35.473 :         3   : Performance
b 35.473 :         4   : High performance
b 35.473 : 
b 35.523 : > help prio:
b 35.523 :   prio                      : show user HPPLMN list.
b 35.523 :   prio <plmn> [<plmn2> ...] : set plmn(s) as user HPPLMN list.
b 35.523 : 
b 35.573 : > help psm:
b 35.573 :   psm <act-time> <tau-time>[h] : request PSM times.
b 35.573 :      <act-time>    : active time in s.
b 35.573 :      <tau-time>    : tracking area update time in s.
b 35.573 :      <tau-time>h   : tracking area update time in h.
b 35.573 :   psm normal       : PSM handled by application.
b 35.573 :   psm              : show current PSM status.
b 35.573 : 
b 35.623 : > help rai:
b 35.623 :   rai off|on : enable or disable RAI.
b 35.624 :   rai        : show current RAI status.
b 35.624 : 
b 35.674 : > help reboot:
b 35.674 :   reboot     : reboot device <last> (forced).
b 35.674 :   reboot <n> : reboot device <n>, if <last> was not the same <n>.
b 35.674 : 
b 35.724 : > help remo:
b 35.724 :   remo   : show current reduced mobility mode.
b 35.724 :   remo 0 : no reduced mobility.
b 35.724 :   remo 1 : reduced mobility (nordic).
b 35.724 :   remo 2 : no reduced mobility.
b 35.724 : 
b 35.774 : > help rscan: remote network scan
b 35.774 :   rscan        : repeat previous network scan.
b 35.774 :   rscan 0      : displays neighbor cell history
b 35.774 :   rscan 1      : start neighbor cell search
b 35.774 :   rscan 2      : start neighbor cell search, all bands
b 35.774 :   rscan 3 <n>  : displays cell history
b 35.774 :   rscan 4 <n>  : start cell search
b 35.774 :   rscan 5 <n>  : start cell search, all bands
b 35.774 :   <n>         : maximum cells to list, values 2 to 15.
b 35.774 : 
b 35.824 : > help scan: start network scan
b 35.824 :   scan        : repeat previous network scan.
b 35.824 :   scan 0      : displays neighbor cell history
b 35.824 :   scan 1      : start neighbor cell search
b 35.824 :   scan 2      : start neighbor cell search, all bands
b 35.824 :   scan 3 <n>  : displays cell history
b 35.825 :   scan 4 <n>  : start cell search
b 35.825 :   scan 5 <n>  : start cell search, all bands
b 35.825 :   <n>         : maximum cells to list, values 2 to 15.
b 35.825 : 
b 35.875 : > help send:
b 35.875 :   send            : send application message.
b 35.875 :   send <message>  : send provided message.
b 35.875 : 
b 35.925 : > help sendflags:
b 35.925 :   sendflags                  : read coap sendflags.
b 35.925 :   sendflags <flags>          : set coap sendflags.
b 35.925 :             <flags>          : flags in decimal.
b 35.925 :             <0xflags>        : flags in hexadecimal.
b 35.925 :   sendflags <id> [<id2> ...] : set coap from names.
b 35.925 :             nores            : request without response (flag 1).
b 35.925 :             init             : initial infos (flag 2).
b 35.925 :             min              : minimal infos (flag 4).
b 35.925 :             dev              : device info (flag 16).
b 35.925 :             sim              : sim-card info (flag 32).
b 35.925 :             net              : network info (flag 64).
b 35.925 :             stat             : network statistics (flag 128).
b 35.925 :             env              : environment info (flag 512).
b 35.925 :             scan             : network scan result (flag 2048).
b 35.925 : 
b 35.975 : > help set:
b 35.975 :   set <key> <value>    : set value to key.
b 35.975 : 
b 35.026 : > help sethex:
b 35.026 :   set <key> <hex-value> : set hexadecimal value for key.
b 35.026 : 
b 36.076 : > help sms:
b 36.076 :   sms                  : receive sms (120s).
b 36.076 :   sms <dest> <message> : send sms and receive sms (120s).
b 36.076 :   <dest>               : international IMSI
b 36.076 :   <message>            : message
b 36.076 : 
b 36.126 : > help timeout:
b 36.126 :   timeout        : read initial coap timeout.
b 36.126 :   timeout <time> : set initial coap timeout in seconds.
b 36.126 : 
I 36.176 : > help unlock:
I 36.176 :   unlock <password>  : unlock protected cmds for 60s.
I 36.176 : 
I 36.226 : > help update:
I 36.226 :   update          : start update download and reboot to apply it.
I 36.226 :   update download : start update download.
I 36.226 :   update info     : display current update info.
I 36.226 :   update erase    : erase current update.
I 36.226 :   update revert   : revert last update.
I 36.226 :   update reboot   : reboot to apply update.
I 36.226 :   update verify   : mark image as verified.
```

Some commands my be disabled depending on the buidl, e.g. `update` (update via UART) will not be available for production images. Therefore check with `help`, which commands are available for your build.

** !!! Under Construction !!! **

