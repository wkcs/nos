#!/bin/bash

JlinkScript="./flash.jlink"

if [ -f $JlinkScript ]; then
    rm $JlinkScript
fi
touch $JlinkScript
echo h > $JlinkScript
echo loadfile $1 >> $JlinkScript
echo r >> $JlinkScript
echo g >> $JlinkScript
echo r >> $JlinkScript
echo exit >> $JlinkScript

JLinkExe -device STM32F103ZET6 -autoconnect 1 -if SWD -speed 4000 -CommanderScript $JlinkScript
