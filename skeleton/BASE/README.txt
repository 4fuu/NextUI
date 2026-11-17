NextUI (formerly MinUI Next), a custom OS based of MinUI with screen sync fixes and many many more features!

Source:
https://github.com/4fuu/NextUI

----------------------------------------
Installing

PREFACE

NextUI has two essential parts: an installer/updater zip archive named "MinUI.zip" and a bootstrap file or folder with names that vary by platform.

On devices that support two SD cards NextUI will generally end up being installed on the "secondary" one. All instances of "SD card" or "primary card" refer to the card that goes into the second slot or to the sole SD card of devices that only support a single card. 

Please refer to https://github.com/4fuu/NextUI for detailed instructions that might differ slightly between devices.

The primary card should be a reputable brand and freshly formatted as FAT32 (MBR).

CAVEATS

While NextUI can be updated from any device once installed, some devices require (minor) changes to NAND or TF1 (via the aforementioned bootstrap file or folder) and therefore need to be installed from the specific device before using. The same is true when trying to use an existing card in a new device of the same type. When in doubt, follow the installation instructions; if all the necessary bits are already installed, the installer will just act as an updater instead.

ALL

Copy the "Roms" and "Saves" folders to the root of your primary card.

MAGICX MINI ZERO 28

NextUI is meant to be used with Moss installed on the SD card that goes into the left slot (labeled TF1/INT). Download and flash the latest version:

	https://github.com/shauninman/Moss-zero28/releases

Copy the "magicx" folder and "MinUI.zip" (without unzipping) to the root of the SD card that goes into the right slot (labeled TF2/EXT).

TRIMUI SMART PRO / TRIMUI BRICK

Copy the "trimui" folder and "MinUI.zip" (without unzipping) to the root of the SD card.

ANBERNIC RG XX (H700) DEVICES

Copy "MinUI.zip" (without unzipping) to the root of the SD card.

Copy the "dmenu.bin" file (found inside the "h700" folder) to the root of your stock OS card's ROMS partition. You can delete it later to uninstall and return to stock.

IMPORTANT: In the stock OS, set the user interface theme to "old style" (default) — not "MU style 1" or "MU style 2", otherwise NextUI will not start.

MIYOO FLIP

Copy the "miyoo355" folder and "MinUI.zip" (without unzipping) to the root of the SD card. Put the SD card into the right slot (beneath the power button).

----------------------------------------
Updating

ALL

Copy "MinUI.zip" (without unzipping) to the root of the SD card.

----------------------------------------
Shortcuts

TRIMUI SMART PRO / TRIMUI BRICK

  Mute: FN switch (volume and rumble)
  "TrimUI" button: Quick menu
  Select button: Game switcher

TRIMUI BRICK / BRICK PRO / SMART PRO S

  Buttons with no action of their own can be assigned a pak to launch: L3/R3 on the Brick, L4/R4 on the Brick Pro, and HOME on the Brick Pro and Smart Pro S. Assign them under Settings > Assignments. Assignments only apply in the main menu, not in-game. HOME no longer acts as a second menu button.

ANBERNIC RG XX

  Menu button: Quick menu
  Select button: Game switcher

----------------------------------------
Collections

A collection is just a text file containing an ordered list of full paths to paks. These text files live in the "Collections" folder at the root of your SD card, eg. "/Collections/Favourites.txt" might look like this:

  /Tools/tg5040/Clock.pak
  /Tools/tg5040/Files.pak

----------------------------------------
Simple mode

Not simple enough for you (or maybe your kids)? NextUI has a simple mode that hides the Tools folder shortcut. Perfect for handing off to littles (and olds too I guess). Just create an empty file named "enable-simple-mode" (no extension) in "/.userdata/shared/".

----------------------------------------
Advanced

NextUI can automatically run a user-authored shell script on boot. Just place a file named "auto.sh" in "/.userdata/<PLATFORM>/" (eg. `/.userdata/h700/` or `/.userdata/tg5040/`). If you're on Windows, make sure your text editor uses Unix line-endings (eg. `\n`), these devices usually choke on Windows line-endings (eg. `\r\n`).

----------------------------------------
Thanks

To eggs, for his NEON scalers, years of top-notch example code, and patience in the face of my endless questions.

Check out eggs' releases (includes source code): 

  RG35XX https://www.dropbox.com/sh/3av70t99ffdgzk1/AAAKPQ4y0kBtTsO3e_Xlrhqha
  Miyoo Mini https://www.dropbox.com/sh/hqcsr1h1d7f8nr3/AABtSOygIX_e4mio3rkLetWTa
  Trimui Model S https://www.dropbox.com/sh/5e9xwvp672vt8cr/AAAkfmYQeqdAalPiTramOz9Ma

To neonloop, for putting together the original Trimui toolchain from which I learned everything I know about docker and buildroot and is the basis for every toolchain I've put together since, and for picoarch.

Check out neonloop's repos: 

  https://git.crowdedwood.com

To adixial and acmeplus and the entire muOS community, for sharing their discoveries for the h700 family of Anbernic devices.

Check out muOS and Knulli:

	https://muos.dev
	https://knulli.org

To fewt and the entire JELOS community, for JELOS (without which MinUI would not exist on the RGB30) and for sharing their knowledge with this perpetual Linux kernel novice.

Check out JELOS:

  https://github.com/JustEnoughLinuxOS/distribution

To Steward, for maintaining exhaustive documentation on a plethora of devices:

	https://steward-fu.github.io/website/

To Gamma, for his efforts unlocking more performance on the MagicX XU Mini M (before we all realized its RG3562 was surreptitiously an RK3266).

Check out his repos (including GammaOS):

	https://github.com/TheGammaSqueeze/

To BlackSeraph, for introducing me to chroot.

Check out the GarlicOS repos:

	https://github.com/GarlicOS

To Jim Gray, for commiserating during development, for early alpha testing, and for providing the soundtrack for much of MinUI's development.

Check out Jim's music: 

  https://ourghosts.bandcamp.com/music
  https://www.patreon.com/ourghosts/
 
