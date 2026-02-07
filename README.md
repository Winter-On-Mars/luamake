# Luamake
A programmable, (semi)-simple build system for C++ projects, configured with lua.

`luamake` is still in early development, so feel free to look through the code, create pull requests, criticize it, open issues, what have you :)

## Usage
The most basic build script needed for building an executable program is as follows
```lua
function Build(b)
  local exe = b:new_exe({
    name = "a",
    root = "src/main.cpp",
    compiler = b.clang({}),
    install_dir = "build"
  })
  return b.install_exe(exe)
end
```
where root is the path to the C++ file with `main` function, relative to the `luamake.lua` file.

When invoking `luamake`, you do so in the root directory of the project, where the `luamake.lua` file is located, everything in the `luamake.lua` file should be relative to it.

## Installing
In whatever directory you want to put the `luamake` executable, you'll just need to run
```sh
git clone https://github.com/Winter-On-Mars/luamake

cd luamake
mkdir build
git submodule update
make release -j
```
The `luamake` file will be in the build directory, so just add it to your path however you want to.

Something like this should work for most people using bash
```sh
export PATH = "$(pwd):$PATH"
```
assuming you are in the place you just cd'ed into the place you cloned `luamake` into.

Then if you'd like to shorten the name (it's what I do) you can edit your `.bashrc` (or `.config/fish/config.fish` if you're a cool person)
```.bashrc
...
alias lm="luamake"
```

## Warnings
* `luamake` is not in stable 1.0, all of this is subject to change :).
* For now we make use of FNV-1a for hashing (which is used in the caching process), as it is **not** a cryptographic hashing function.
  * You should probably add 'build/__luamake_cache/*' to your .gitignore (or just add build/*) in general, but this is a warning :).
* `luamake` uses lua script files to program the build system. Make sure to read the script before running it, the usual warning about running scripts that other people make.
