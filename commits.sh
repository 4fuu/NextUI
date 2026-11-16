#!/bin/bash

show() {
	pushd "$1" >> /dev/null
	HASH=$(git rev-parse --short=8 HEAD)
	NAME=$(basename $PWD)
	DATE=$(git log -1 --pretty='%ad' --date=format:'%Y-%m-%d')
	REPO=$(git config --get remote.origin.url)
	REPO=$(sed -E "s,(^git@github.com:)|(^https?://github.com/)|(.git$)|(/$),,g" <<<"$REPO")
	popd >> /dev/null

	printf '\055 %-24s%-10s%-12s%s\n' $NAME $HASH $DATE $REPO
}
list() {
	pushd "$1" >> /dev/null
	for D in ./*; do
		show "$D"
	done
	popd >> /dev/null
}
rule() {
	echo '----------------------------------------------------------------'	
}
tell() {
	echo $1
	rule
}

bump() {
	printf '\n'
}

{
	# tell NEXTUI
	printf '%-26s%-10s%-12s%s\n' NEXTUI HASH DATE USER/REPO
	rule
	show ./
	bump
	
	tell TOOLCHAINS
	list ./toolchains
	bump
	
#	tell RG35XX
#	show ./workspace/rg35xx/other/DinguxCommander
#	show ./workspace/rg35xx/other/evtest
	
#	tell MIYOOMINI
#	show ./workspace/miyoomini/other/DinguxCommander
#	show ./workspace/miyoomini/other/sdl
	
#	tell TRIMUISMART
#	show ./workspace/trimuismart/other/DinguxCommander
#	show ./workspace/trimuismart/other/unzip60
	
#	tell RGB30
#	show ./workspace/rgb30/other/DinguxCommander

	tell TG5040
	show ./workspace/tg5040/other/evtest
	show ./workspace/tg5040/other/jstest
	show ./workspace/tg5040/other/unzip60
	
#	tell M17
	
#	tell RG35XXPLUS
#	show ./workspace/rg35xxplus/other/dtc
#	show ./workspace/rg35xxplus/other/fbset
#	show ./workspace/rg35xxplus/other/sdl2
#	show ./workspace/rg35xxplus/other/unzip60
	
#	tell GKDPIXEL
	
#	tell MY282
#	show ./workspace/my282/other/unzip60
#	show ./workspace/my282/other/DinguxCommander-sdl2
	
#	tell MAGICMINI
#	show ./workspace/magicmini/other/351files
	
#	tell ZERO28
#	show ./workspace/zero28/other/DinguxCommander-sdl2
	
#	tell MY355
#	show ./workspace/my355/other/evtest
#	show ./workspace/my355/other/mkbootimg
#	show ./workspace/my355/other/rsce-go
#	show ./workspace/my355/other/DinguxCommander-sdl2
	
	tell CHECK
	echo https://github.com/USER/REPO/compare/HASH...HEAD
	bump
} | sed 's/\n/ /g'