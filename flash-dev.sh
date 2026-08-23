#!/bin/sh

rev=`grep -o -e 'revision: v.*' west.yml | cut -d' ' -f2`

if [ "${rev}" \< "v2.7.0" ] ; then

echo "${rev} (legacy build)"
full_image="zephyr/merged.hex"

elif [ "${rev}" \< "v3.4.0" ] ; then

echo "${rev} (sysbuild, single file)"
full_image="merged.hex"

else 

echo "${rev} (sysbuild, seperate files)"
mcu_image="mcuboot/zephyr/zephyr.hex"
app_image="coaps-client/zephyr/cloudcoap.signed.hex"
full_image="merged.hex"

fi

build=build_nrf9160dk_nrf9160_ns
target=nRF9160_xxAA
tool=nrfutil
reset=""

echo "$1"
case $1 in
  "dk")
    build=build_nrf9160dk_nrf9160_ns
    ;;
  "dk2")
    build=build_nrf9161dk_nrf9161_ns
    ;;
  "dk3")
    build=build_nrf9151dk_nrf9151_ns
    reset="nrfutil device reset --reset-kind RESET_PIN"
    ;;
  "mikroe")
    build=build_nrf9160dk_nrf9160_ns
    ;;
  "thingy")
    build=build_thingy91_nrf9160_ns
    ;;
  "thingyx")
    build=build_thingy91x_nrf9151_ns
    reset="nrfutil device reset --reset-kind RESET_PIN"
    ;;
  "feather")
    build=build_feather_nrf9160_ns
    ;;
  "feather61")
    build=build_feather_nrf9161_ns
    tool=probe-rs
    ;;
  "feather51")
    build=build_feather_nrf9151_ns
    tool=probe-rs
    target=nRF9151_xxAA
    ;;
  "feather51py")
    build=build_feather_nrf9151_ns
    tool=pyocd
    target=nrf91
    reset="pyocd reset --target ${target}"
    ;;
  "conexio")
    build=build_conexio_stratus_pro_nrf9161_ns
    ;;
  "conexio51")
    build=build_conexio_stratus_pro_nrf9151_ns
    reset="nrfutil device reset --reset-kind RESET_PIN"
    ;;
  "md51")
    build=build_makerdiary_nrf9151_ns
    tool=pyocd
    reset="pyocd reset --target ${target}"
    ;;
  *)
    echo "target $1 unknown! Use: (dk|dk2|dk3|mikroe|thingy|thingyx|feather|feather61|feather51|conexio|conexio51|md51)"
    exit 1
    ;;
esac

if [ -n "${mcu_image}" ] ; then
    echo "merge ${build}"
    echo "      ${mcu_image} ${app_image} to ${full_image}"
    python3 extras/hexmerge.py -o ${build}/${full_image} ${build}/${mcu_image} ${build}/${app_image}
fi
 
if [ "${tool}" = "nrfutil" ] ; then
   if [ -z "$2" ] ; then 
     cmd="nrfutil device program --core application --firmware ${build}/${full_image} --options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,verify=VERIFY_READ,reset=RESET_DEFAULT"
   else
     cmd="nrfutil device program --core application --firmware ${build}/${full_image} --options chip_erase_mode=ERASE_ALL,verify=VERIFY_READ,reset=RESET_DEFAULT"
   fi
   cmd2=""
elif [ "${tool}" = "probe-rs" ] ; then
   if [ -z "$2" ] ; then 
      cmd="probe-rs download --chip ${target} --binary-format hex ${build}/${full_image}"
   else
      cmd="probe-rs download --chip-erase --chip ${target} --binary-format hex ${build}/${full_image}"
   fi
   cmd2="probe-rs reset --chip ${target}"
elif [ "${tool}" = "pyocd" ] ; then
   if [ -z "$2" ] ; then 
      cmd="pyocd load --target ${target} --format hex ${build}/${full_image}"
   else
      cmd="pyocd load -e chip --target ${target} --format hex ${build}/${full_image}"
   fi   
   cmd2=""
else
   echo "unknown tool '${tool}'"
fi

set -x
${reset};${cmd};${cmd2}

