# Shuzi 
#### Suzi is a super minimal lightweight image viewer designed to quickly look at images through the terminal or with a minimal GUI
##### Suzi is written in C with SDL2. It is around 400 lines of code.
##### Consult your local package manager for help. These are the core dependencies and I'll add some examples for certain distros.
##### There is also a written guide on compiling and building from source

##### Needed:
###### -> build-essential
###### -> pkgconf
###### -> SDL2 (Dev)
###### -> SDL2 Image
###### -> zenity (Optional but recommended for the file picker)

### Installing Dependencies

##### Arch Linux
``
sudo pacman -S base-devel pkgconf sdl2-compat sdl2_image
``

##### Debian / Ubuntu (Unsure)
``
sudo apt install build-essential pkg-config libghc-sdl2-dev libghc-sdl2-image-dev
``

##### Fedora
``
sudo dnf group install development-tools
sudo dnf install sdl2-compat SDL2_image
``

##### Gentoo
``
(sudo/doas) emerge media-libs/libsdl2 media-libs/sdl2-image
``

##### Alpine and Chimera
``
doas apk add sdl2-compat-devel sdl2-image-devel
``

##### Void Linux
``
sudo xbps-install base-devel pkgconf SDL2-devel SDL2_image-devel
``


### Building from source
````
git clone https://github.com/LuxymillianXYZ/shuzi.git
cd shuzi
make
sudo cp shuzi /usr/local/bin/shuzi
````

#### Optionally you can also use make options `` install `` to automatically add it to your path

### Usage
#### You can always do ``shuzi -h`` to list the options but here is a cheat sheet here
````
usage: shuzi [image|directory ...]
  with no arguments, opens an empty window:
  drag an image onto it, or press O to pick files.

keys:
  q / Esc        quit            o            open file chooser
  n / Right / Spc next image      p / Left / Bksp  previous image
  Home / End     first / last image
  + / = / wheel  zoom in          - / wheel        zoom out
  0              100% (actual size)
  f / Enter      fit to window
  arrows         pan (also drag with the mouse)
  F11            toggle fullscreen
````

