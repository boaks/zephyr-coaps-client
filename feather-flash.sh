#!/bin/sh

date

zephyr-tools -b
newtmgr -c vscode-zephyr-tools image upload build_feather_nrf9160_ns/zephyr/app_update.bin -r 3 -t 2
newtmgr -c vscode-zephyr-tools reset

