- [ ] add multthreading for the compilation step
  - going to wait till the c rewrite to impl this
- [ ] add config option to switch caching algorithm
  - [ ] add other hashing algorithms
- [ ] issue where we're writing the cache to the file even if there's a compilation failure, so we should fix that some how :)
  - will probably be fixed by adding other hashing functions
- [ ] update the help message
- [x] fix cleaning with `-c`
  - [ ] add option to write something like `-c file1 file2` to only remove those files from the cache
