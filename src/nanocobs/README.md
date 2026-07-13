# nanocobs

This directory vendors `cobs.c` and `cobs.h` from
`charlesnicholson/nanocobs`.

The pinned upstream commit is recorded in `REVISION`. Run `./sync.sh` from this
directory to refresh the vendored files from the latest commit on `main`, or
pass an explicit branch name or commit hash:

```bash
./sync.sh
./sync.sh --branch develop
./sync.sh --revision b50277dd96a73c586f7a8888bdfdf5c5d1cb0651
./sync.sh --branch main --revision b50277dd96a73c586f7a8888bdfdf5c5d1cb0651
```
