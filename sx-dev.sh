#!/bin/sh

rev=`grep -o -e 'revision: v.*' west.yml | cut -d' ' -f2`

if [ "${rev}" \< "v2.7.0" ] ; then

echo "${rev} (legacy build)"
app_image="zephyr/app_update.bin"

else 

echo "${rev} (sysbuild)"
app_image="coaps-client/zephyr/cloudcoap.signed.bin"

fi

build=build_nrf9160dk_nrf9160_ns
dev=/dev/ttyACM0
sxopt=-k

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
    ;;
  "mikroe")
    build=build_nrf9160dk_nrf9160_ns
    dev=/dev/ttyUSB0
    ;;
  "thingy")
    build=build_thingy91_nrf9160_ns
    ;;
  "thingyx")
    build=build_thingy91x_nrf9151_ns
    ;;
  "feather")
    build=build_feather_nrf9160_ns
    dev=/dev/ttyUSB0
    ;;
  "feather61")
    build=build_feather_nrf9161_ns
    ;;
  "feather51")
    build=build_feather_nrf9151_ns
    ;;
  "conexio")
    build=build_conexio_stratus_pro_nrf9161_ns
    dev=/dev/ttyUSB0
    ;;
  "conexio51")
    build=build_conexio_stratus_pro_nrf9151_ns
    dev=/dev/ttyUSB0
    ;;
  "md51")
    build=build_makerdiary_nrf9151_ns
    sxopt=
    ;;
  *)
    echo "target $1 unknown! Use: (dk|dk2|dk3|mikroe|thingy|thingyx|feather|feather61|feather51|conexio|conexio51|md51)"
    exit 1
    ;;
esac

if [ "$2" ]  ; then
   dev="$2"
fi

echo "dev  : ${dev}"

sx ${sxopt} ${build}/${app_image} < $dev > $dev


