#!/bin/sh

image="build_feather_nrf9160_ns"
type="fsol"

. ./s3put-fw.sh 

upload

config cali.351516178721254 cali.351516178721502

