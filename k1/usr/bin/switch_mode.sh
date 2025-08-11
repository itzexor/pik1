#!/bin/sh
#FIXME: this ip is hardcoded
stock() {
  #mv /etc/init.d/_S50webcam /etc/init.d/S50webcam
  mv /etc/init.d/_S50nginx_service /etc/init.d/S50nginx_service
  mv /etc/init.d/_S55klipper_service /etc/init.d/S55klipper_service
  mv /etc/init.d/_S56moonraker_service /etc/init.d/S56moonraker_service
  mv /etc/init.d/S99pik1 /etc/init.d/_S99pik1  sync
}

pi() {
  #mv /etc/init.d/S50webcam /etc/init.d/_S50webcam
  mv /etc/init.d/S50nginx_service /etc/init.d/_S50nginx_service
  mv /etc/init.d/S55klipper_service /etc/init.d/_S55klipper_service
  mv /etc/init.d/S56moonraker_service /etc/init.d/_S56moonraker_service
  mv /etc/init.d/_S99pik1 /etc/init.d/S99pik1
}

$1
