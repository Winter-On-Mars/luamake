# Luamake
A programmable, (semi)-simple build system for C++ projects, configured with lua.

`luamake` is still in early development, so feel free to look through the code, create pull requests, criticize it, open issues, what have you :)

## Warnings
* `luamake` is not in stable 1.0, all of this is subject to change :).
* For now I make use of FNV-1a for hashing (which is used in the caching process), and it is **not** a cryptographic hashing function.
  * You should probably add 'build/__luamake_cache/*' to your .gitignore (or just add build/*) in general, but this is a warning :).
* `luamake` uses lua script files to program the build system. Make sure to read the script before running it, the usual warning about running scripts that other people make.

# Installation
## Dependencies
* lua-devel (obviously)
  * Version >= 5.4 (could probably work with 5.3, we just need integers and bitwise operations for the hashing)
  * I'll probably do a git sub-module or something to have the lua vm be included in the project so that you can build the project from a single `make` file, but as of now we depend on system libs.

## Script
```sh
git clone https://github.com/Winter-On-Mars/luamake
```
Then just add it to your path however you want to

If you installed the program into `$HOME/dev/luamake`, then you'd run
```sh
export PATH = "$HOME/dev/luamake:$PATH"
```

Then if you'd like to shorten the name (it's what I do) you can edit your .bashrc (or .config/fish/config.fish if you're a cool person)
```.bashrc
...
alias lm="luamake"
```

# Configuration
## `Build`


-- Warning this section is out of date, we are working on updating it to the new standard (so basically just completely ignore this section and its sub-sections)
# Configuration
The ``config.lua`` should be placed at the root of the project, or where ever you intend to call `luamake`.

## Config Layout
Smallest Configuration needed :)
```lua
local config = {
    compiler = "clang++",
    flags = {},
    bin_name = 'a',
}

return config
```

The config fields `compiler` and `flags` must be set (even if `flags` is an empty table).

Here is the types for every (current) configuration option
```lua
---@type { compiler: string, flags: string[], bin_name: string, linking: string[]?, linking_dir: string?, pre_exec: string[]?, post_exec: string[]?}
local config = { }
```

### `compiler`
Path of the compiler to be invoked (ex. "clang++", "g++", etc)

### `flags`
List of the compiler flags to be passed to the compiler
#### Example
```lua
flags = {
    "std=c++17",
    "Wall",
    "Wpedantic",
    "Wconversion",
    "Werror",
}
```

### `bin_name`
Name of the output binary.

### `linking`
List of the external dependencies that you are linking against
#### Example
```lua
linking = {
    "SDL2"
}
```

### `linking_dir`
Flag to specify dir to search for linking
* This should probably be an array but I don't really know how this work.

### `pre_exec`
List of strings that will be run in the shell before any of the files are compiled
  * Can be used to precompile shaders (but we might just want to add something later to watch certain files for changes(?))

### `post_exec`
List of strings that will be run after all of the files are compiled (including the output binary)
  * Helpful for running the program afterwards after it finishes

# Expected Project Layout
```
://res
|- config.lua
|- build
|  |- helper.o
|  |- main.o
|  |- a.out
|- src
   |- helper.cpp
   |- helper.hpp
   |- main.cpp
```
