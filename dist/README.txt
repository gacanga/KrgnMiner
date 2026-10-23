KrgnMiner - NVIDIA GPU miner for Kerrigan (KRGN), Equihash 192,7 "kerrigan"
Windows x64, CUDA 12.9 build (native code for Pascal sm_61: P104-100, GTX 1070/1080)

Open source (Apache-2.0), NO developer fee.
Pool: rplant (stratum-eu.rplant.xyz:7040).
Every GPU solution is checked again on the CPU before it is sent to the pool.

QUICK START
  1. Open krgnminer.conf and replace YOUR_KRGN_ADDRESS with your Kerrigan address (starts with K)
     ("rig1" is the worker name, you can change it).
  2. Double-click start_krgn.bat. It restarts the miner automatically and
     writes krgnminer.log; close the window to stop.

Other scripts
  test.bat   correctness tests (CPU, then every GPU against reference solutions)
  bench.bat  60 s Sol/s benchmark (stop mining first)

Requirements
  - Windows 10/11 x64
  - NVIDIA driver with CUDA 12 support (R525 or newer; R580 is the last branch
    for Pascal). Nothing else to install.
  - about 5.4 GB of free GPU memory per card
  - Newer GPUs run the PTX code through the driver's JIT (works, not tuned).

Measured: 3 x P104-100, stock settings: 42-43 Sol/s (~14 Sol/s per card).

Files: krgnminer.exe, kgpu.dll, cudart64_12.dll (NVIDIA CUDA runtime), ca-bundle.crt
(CA certificates for TLS pools), krgnminer.conf, start_krgn.bat, test.bat, bench.bat,
LICENSE, NOTICE.
Command line: krgnminer.exe --help

Source code, documentation, releases: https://github.com/gacanga/KrgnMiner
