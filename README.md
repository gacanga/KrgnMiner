# KrgnMiner — NVIDIA GPU miner for Kerrigan (KRGN), Equihash 192,7

KrgnMiner mines [Kerrigan](https://kerrigan.network/) (KRGN) on NVIDIA GPUs with **Equihash(192,7)** and the
BLAKE2b personalization `kerrigan`, on the [rplant](https://pool.rplant.xyz/) Zcash-style stratum.
The CUDA solver (Wagner's algorithm) was written from scratch for Pascal cards (P104-100, GTX 1070/1080, sm_61).

- **No dev fee.**
- Open source, Apache-2.0.
- Windows x64 release built with CUDA 12.9 (sm_61 code + PTX, so newer GPUs run it through the driver's JIT).
- Every solution found by the GPU is checked again on the CPU (collisions, ordering, distinct indices) before it
  is sent to the pool.

Version française : [README.fr.md](README.fr.md).
The same solver code base is used by [YecMiner](https://github.com/gacanga/YecMiner) (Ycash, Equihash 192,7) and
[NullMiner](https://github.com/gacanga/NullMiner) (NULL network, Equihash 144,5).

## Quick start (Windows)

1. Download the zip from [Releases](https://github.com/gacanga/KrgnMiner/releases) and extract it.
2. Open `krgnminer.conf` and replace `YOUR_KRGN_ADDRESS` with your Kerrigan address (it starts with `K`);
   `rig1` is the worker name, change it if you like.
3. Double-click `start_krgn.bat`. It restarts the miner 10 s after any exit and logs to `krgnminer.log`;
   close the window to stop.

`test.bat` runs the correctness tests on every GPU, `bench.bat` a 60 s Sol/s benchmark (stop mining first).

Requirements: Windows 10/11 x64, an NVIDIA driver with CUDA 12 support (R525 or newer; R580 is the last branch for
Pascal), about **5.4 GB of free GPU memory per card** (8 GB cards). Nothing else needs to be installed.

## Pool

rplant, Kerrigan Equihash 192,7 port: `stratum+tcp://stratum-eu.rplant.xyz:7040`
(TLS: `stratum+ssl://stratum-eu.rplant.xyz:17040`, fallback `stratum-ru.rplant.xyz`), user `KRGN_ADDRESS.rig`,
password `x`, 1 % fee, PROP, minimum payment 1 KRGN.

Protocol (NOMP Equihash):

```
-> mining.subscribe [agent]                  <- [session, extranonce1]
-> mining.authorize [user, pass]             <- true
<- mining.set_target [target_hex_be]         (or mining.set_difficulty [d], diff 1 = 0x0007ffff << 224)
<- mining.notify [job, version, prevhash, merkleroot, reserved, time, bits, clean, "192_7", "kerrigan"]
-> mining.submit [user, job, time, nonce2, "fd9001" + solution (400 bytes)]
```

The 32-byte nonce is extranonce1 followed by nonce2. A share is valid when
SHA256d(header || fd9001 || solution) is below the share target.

## Configuration

The miner reads `krgnminer.conf` next to the executable (or one folder above it), or the file given with
`--config`. Each line is `key = value`; `#` and `;` start a comment. Every key is also a command-line option
(`--key value`); command-line values override the file, and `-o` on the command line replaces the pools of the file.

| Option / key | Default | Meaning |
| --- | --- | --- |
| `-o`, `--pool`, `--url` / `pool` | — | Pool URL, repeatable (failover order). `stratum+tcp://` = plain TCP, `stratum+ssl://` or `stratum+tls://` = TLS |
| `-u`, `--user` / `user` | — | `KRGN_ADDRESS.rig` |
| `-p`, `--pass` / `pass` | `x` | Pool password |
| `--gpus`, `--devices` / `gpus` | `all` | `all` or a list such as `0,1,2` (PCI bus order, as nvidia-smi) |
| `--tpb` / `tpb` | `256` | Threads per block of the collision kernels, multiple of 32 (32–1024) |
| `--stats` / `stats` | `30` | Seconds between summary lines (minimum 5) |
| `--log` / `log` | — | Append the log to this file |
| `--cafile` / `cafile` | `ca-bundle.crt` next to the exe | CA bundle for TLS pools |
| `--config`, `-c` | `krgnminer.conf` | Configuration file |
| `--args-file FILE` | — | Read more arguments from a file (whitespace separated) |
| `--test` | | Correctness tests (CPU, then every selected GPU), exit code 0 when all pass |
| `--bench [s]` | 60 s | Sol/s benchmark on the selected GPUs |
| `--profile` | | Time of every solver stage (average of 3 runs) |
| `--help`, `-h` | | Usage |

Exit codes used by the restart loop: 2 = bad argument or missing pool/user, 3 = CUDA error, 4 = a GPU run did not
return for 20 s (unstable clocks?), 5 = test/benchmark timeout.

## Performance (measured)

NVIDIA P104-100 8 GB, stock settings, rplant pool:

| Setup | Sol/s |
| --- | --- |
| 3 × P104-100 | 42–43 Sol/s (≈ 14 Sol/s per card) |

Over a 4-hour pool session: 103 shares accepted, none rejected.
The solver is limited by memory bandwidth (scattered 32-byte accesses).
With the same solver, YecMiner measured 68 Sol/s on 5 × P104-100 at stock settings (see the YecMiner README).

## How it works

Wagner's algorithm on 2^25 strings of 24 bytes (2^24 BLAKE2b-384 hashes, two strings each):

- 6 collision steps on 24-bit digits, then a final step on the last 48 bits, then the reconstruction of the
  128 indices of each candidate;
- every level is stored in 4096 buckets × 8704 slots; inside a bucket the remaining bits are matched with linked
  lists in shared memory, with the link and pairing phases fused;
- each string is one record holding its data and its tree entry (32, 24 or 16 bytes depending on the level),
  because a scattered write costs one 32-byte sector on this GPU however few bytes are written;
- about 5.4 GB of GPU memory per card.

The host computes the BLAKE2b midstate of the 140-byte header; the GPU finishes the hashes. The same phase
functions (`gpu/eq_common.h`) are compiled for CUDA (`gpu/eq_gpu.cu`) and as a sequential CPU emulation used to
generate the test vectors. Compile-time layout options are described at the top of `gpu/eq_common.h`.

## Building from source (Windows)

Prerequisites, as expected by the scripts:

- CUDA Toolkit **12.9** (`CUDA_PATH_V12_9`, default `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9`);
  CUDA 13 cannot target Pascal.
- Visual Studio 2022 (or Build Tools) with the C++ x64 tools (found with `vswhere`).
- MSYS2 in `C:\msys64` with the MINGW64 packages `mingw-w64-x86_64-gcc` and `mingw-w64-x86_64-openssl`.

| Script | What it does |
| --- | --- |
| `build_all.bat` | Builds `dev\kgpu.dll` (nvcc + MSVC, `windows\build_gpu_dll.bat`) and `dev\krgnminer.exe` (MSYS2 g++, `windows/build_host.sh`), then runs the tests. `build_all.bat nobuild` = tests only |
| `run_test.bat` | `dev\krgnminer.exe --test`, log `logs\test.log` |
| `run_bench.bat` | 60 s benchmark of `dev\`, log `logs\bench.log`; extra arguments in `run\bench_args.txt`; with `run\variants.txt`, builds and profiles layout variants (`windows\build_variants.bat`) |
| `deploy.bat` | Copies `dev\` to `bin\`, only if the last `logs\test.log` says `TEST RESULT: ALL OK` |
| `krgn_pool.bat` | Mines with `bin\krgnminer.exe` and `krgnminer.conf`, restart loop, log `logs\pool.log`; extra arguments in `run\pool_args.txt` |
| `stop_krgn.bat` | Stops the restart loop and `krgnminer.exe` |

Build options (environment variables): `KM_CRT=/MT` links the C runtime statically into `kgpu.dll`,
`KM_NVCC_EXTRA` (or `run\nvcc_extra.txt`) adds nvcc flags, `KM_HOST_ARCH` replaces `-march=native`. For a
portable release build use `set KM_CRT=/MT` and `set KM_HOST_ARCH=-march=x86-64 -mtune=generic` before
`build_all.bat`. The release zip contains `krgnminer.exe`, `kgpu.dll`, `cudart64_12.dll`, `ca-bundle.crt` and the
files of [`dist/`](dist).

## Tests

`krgnminer.exe --test`:

1. CPU: BLAKE2b-384 `kerrigan` known answers (from Python's hashlib), on both hashing code paths;
2. CPU: the reference solutions in `src/eq_vectors.h`, produced by Kerrigan's own solver
   (Kerrigan Core `src/crypto/equihash.cpp`), must validate and re-encode identically, and corrupted or
   misordered variants must be rejected;
3. every selected GPU: the 5 reference headers (from a real rplant job, varied nonces) must give exactly the
   reference solutions and exactly the per-level string counts of the CPU emulation, then 10 random nonces with
   every solution checked by the CPU.

`tests/eq_cloud_test.cpp` (Linux, libsodium) checks the host code and the CPU emulation against libsodium and
the reference solutions; `tools/gen_vectors.py` regenerates `src/eq_vectors.h` from its output (the reference
solutions themselves come from a build of Kerrigan's solver, which is not included). Example:

```sh
g++ -O2 -std=gnu++17 -Isrc -Igpu -o eq_cloud_test tests/eq_cloud_test.cpp src/eq_host.cpp -lsodium
./eq_cloud_test <header140hex> <reference solution hex>...
```

`tools/stratum_probe.ps1` connects to a stratum port and logs the raw lines (no mining, no share).

## Credits and licence

- Solver, host code and stratum client: gacanga. The stratum client (`src/json.h`, `src/tls_conn.*`) comes from
  FiloMiner (Apache-2.0).
- Kerrigan Core (MIT) was used as the correctness reference for the tests only; it is not linked into the miner.

Apache License 2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE). No warranty: you mine at your own risk, and
mining uses electricity.
