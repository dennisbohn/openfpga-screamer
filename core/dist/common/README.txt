Screamer for the Analogue Pocket - game files

The core needs the files of your own copy of Screamer (1995). Copy these folders and files
from the game's install folder into this folder, as they are:

  GFX          the whole folder
  DATA         the whole folder
  SCREAM.gog   the CD image with the game's music
  SCREAM.DAT   the cue sheet of the CD image

This folder is Assets/screamer/common/ on the SD card. The core is tested with the GOG version.
If a file is missing, the core lists the missing files when it starts.

Optional: a text file screamer.cfg in this folder changes how far the core draws. Each number is a
factor on the original's distance; without the file the core uses these:

  far = 2.0     the view distance
  lod = 1.2     where things switch to a coarser mesh (off: never)

https://github.com/dennisbohn/openfpga-screamer
