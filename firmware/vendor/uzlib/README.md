# uzlib decompression subset

Source: https://github.com/pfalcon/uzlib/tree/master/src
Snapshot downloaded 19 October 2026. Copyright Joergen Ibsen and Paul Sokolovsky; zlib licence preserved in source and LICENSES/uzlib.txt.

Only inflate and checksums are compiled. `uzlib_conf.h` enables paranoid input checks; other source files are unmodified. PNG notifications use the size-checked output buffer as decompression history, avoiding a fixed 32 KiB dictionary.

Snapshot hashes (before the local configuration change):
- `adler32.c`: `635a9425f674a9caf205de9742eb4e7bec9723ffb83a2b0e7d9bd67530d9904e`
- `crc32.c`: `8283a70917f5fff84297076598b56f50ff8670b2547cd611c43e4aada33307b3`
- `defl_static.h`: `4f3356c411cfb4f14cd6eb230751a11786420bbe6ee42128ef81bf173412a89d`
- `tinf.h`: `1061aecf91937b496cd4005a84320ceb10f36b0a0eb1cc1df418b3a92c2809dd`
- `tinf_compat.h`: `c1a53510f4681e5dda20b6fd89422c0c9d26c954189c9cd67e4db0de0ce55165`
- `tinflate.c`: `887a8edf2286d6024a02aa160a07614df4b1859d6d43986fe737237c93b58935`
- `uzlib.h`: `7e11749b52566dddc8f6eddfd8439366fa1b2a503bb6c32777d8b9c82e0dd2ee`
- `uzlib_conf.h`: `02d78f091d8bbd015e80de1e9a3e3fdff46c1e45e2485b8fa8d00f79d18f333d`
