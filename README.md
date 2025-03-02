# Luamake
A programmable, (semi)-simple build system for C++ projects, configured with lua.

`luamake` is still in early development, so feel free to look through the code, create pull requests, criticize it, open issues, what have you :)

## Warnings
* `luamake` is not in stable 1.0, all of this is subject to change :).
* For now I make use of FNV-1a for hashing (which is used in the caching process), and it is **not** a cryptographic hashing function.
  * You should probably add 'build/__luamake_cache/*' to your .gitignore (or just add build/*) in general, but this is a warning :).
* `luamake` uses lua script files to program the build system. Make sure to read the script before running it, the usual warning about running scripts that other people make.

# Installation
You should only have to `init` the git submodules as pointed out in the build script, as lua is treated as a submodule of this project.

## Script
In whatever dir you'll want to add this command in, run
```sh
git clone https://github.com/Winter-On-Mars/luamake

cd luamake
git submodule update
make
```
Then just add it to your path however you want to

something like this should work for most people using bash
```sh
export PATH = "$(pwd):$PATH"
```
assuming you are in the the place you just cd'ed into the place you cloned `luamake` into.

Then if you'd like to shorten the name (it's what I do) you can edit your .bashrc (or .config/fish/config.fish if you're a cool person)
```.bashrc
...
alias lm="luamake"
```

# Configuration
## `Build`
Type: `function(builder: Builder): nil`

You're expected to modify the passed in `builder` object to have the following fields specified
* `type`: `"exe" | "slib" | "dlib"`
  * Specifies this modules output type as one of the following
  * `exe`: Executable program, the output will be the same as specified by `builder.name`.
  * `slib`: Static library, the output will be prefixed with `lib`, with a file extension of `.a` on linux + mac platforms, and `.LIB` extension on windows.
  * `dlib`: Dynamic library, output will be prefixed with `lib`, with it's file extension being `.so`, `.dylib`, or `.dll` on unix, mac, and windows respective platforms.
* `root`: `string`
  * Relative path from where the luamake file is to the root of the project
* `compiler`: TODO
* `name`: `string`
  * Name of the modules output, may be modified by `builder.type` as mentioned above.
* `version`: `string`
  * Defaults to semantic versioning tag of the current program.
* `description`: `string`
  * Description of the module, to be used for searching through deps.

## `Run`
Type = `function(runner: Runner): nil`

## `Test`
A global array of objects that look like
```lua
{
  fun: function(tester: Tester): nil;
  output?: {
    expected: string;
    from: "stdout"|"stderr"|FilePath;
  };
}
```

`fun` is called to set up the testing environment. You are expected to set the following field in the tester object passed in
* `exe`: `string`
  * The program to be called
With the following fields being optional
* `args`: `Array<string>`
  * An array of strings to be passed to the program through command line arguments, in the order specified here

After the program is finished running, if `output` is specified then it will compare the contents of the specified `from` field with that in the `expected` field.


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
