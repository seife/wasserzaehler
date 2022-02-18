#!/bin/bash
# 
# add a script before building to create version.h
# idea from here: https://github.com/fabianoriccardi/git-describe-arduino
PARAM=(--build-property "recipe.hooks.sketch.prebuild.9.pattern={build.source.path}/myversion.sh {build.path}/sketch")
arduino-cli -b esp8266:esp8266:nodemcu compile "${PARAM[@]}" $@
