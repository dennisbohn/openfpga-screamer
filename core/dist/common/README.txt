Screamer for the Analogue Pocket - game files

The core needs the files of your own copy of Screamer (1995). Copy these folders and files
from the game's install folder into this folder, as they are:

  GFX          the whole folder
  DATA         the whole folder
  SCREAM.gog   the CD image with the game's music
  SCREAM.DAT   the cue sheet of the CD image

This folder is Assets/screamer/common/ on the SD card. The core is tested with the GOG version.
If a file is missing, the core lists the missing files when it starts.

Optional: a text file screamer.cfg in this folder sets how far the core draws (the default is
1.5 times the original's distance for both):

  far = 1.5     the view distance
  lod = 1.5     where things switch to a coarser mesh (off: never)
  tiny = 0      things that would be smaller than this many pixels are not drawn (0: all are)
  bench = 0     1: at the start, race every track once and show the frame times (also in debug.txt)

https://github.com/dennisbohn/openfpga-screamer
