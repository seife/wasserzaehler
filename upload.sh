#!/bin/bash
#
# give hostname/IP as parameter for http update...
# better have built with "./compile.sh -v -e" or it won't exist...
if [ -n "$1" ]; then
	curl -v -F "image=@build/esp8266.esp8266.nodemcu/wasserzaehler.ino.bin" "$1"/update
	echo
	exit
fi
# ...else it will update at port ttyUSB0
arduino-cli -b esp8266:esp8266:nodemcu \
	upload \
	--board-options baud=3000000 \
	--port=/dev/ttyUSB0 \
	-v
