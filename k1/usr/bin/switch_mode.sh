#!/bin/sh
#FIXME: this ip is hardcoded
saf() {
  mv /etc/init.d/_S50webcam /etc/init.d/S50webcam
  mv /etc/init.d/_S50nginx_service /etc/init.d/S50nginx_service
  mv /etc/init.d/_S55klipper_service /etc/init.d/S55klipper_service
  mv /etc/init.d/_S56moonraker_service /etc/init.d/S56moonraker_service
  mv /etc/init.d/S99rpi-bridge-init /etc/init.d/_S99rpi-bridge-init
  sed -i s/192\.168\.1\.11/127\.0\.0\.1/ /usr/data/guppyscreen/guppyscreen.json
  sync
}

pi() {
  mv /etc/init.d/S50webcam /etc/init.d/_S50webcam
  mv /etc/init.d/S50nginx_service /etc/init.d/_S50nginx_service
  mv /etc/init.d/S55klipper_service /etc/init.d/_S55klipper_service
  mv /etc/init.d/S56moonraker_service /etc/init.d/_S56moonraker_service
  mv /etc/init.d/_S99rpi-bridge-init /etc/init.d/S99rpi-bridge-init
  sed -i s/127\.0\.0\.1/192\.168\.1\.11/ /usr/data/guppyscreen/guppyscreen.json
}

$1
