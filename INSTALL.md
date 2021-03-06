Debian install of Protoshares Pool Miner (PTS Miner) with GPU mining
====================================================================

Ubuntu 12.04 LTS x86_64
-----------------------
1) Install AMD or NVidia SDK

For NVidia, follow instructions on 

http://docs.nvidia.com/cuda/cuda-getting-started-guide-for-linux/index.html#package-manager-installation

For AMD, follow instructions on

http://developer.amd.com/tools-and-sdks/heterogeneous-computing/amd-accelerated-parallel-processing-app-sdk/

2) Add OpenCL to lib path:

NVidia:

export PATH=/usr/local/cuda-5.5/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-5.5/lib64:$LD_LIBRARY_PATH

(you might add these lines on ~/.bashrc so they are run at every login)

AMD:

Should have been done by the installer, but if not, try:

export LD_LIBRARY_PATH=/opt/AMDAPP/lib/x86_64:$LD_LIBRARY_PATH
 

3) Install other dependencies:

apt-get install build-essential opencl-headers

4) Compile the miner:

cd jhProtominer/src/jhProtominer
make -f makefile.linux

5) Run:

./jhProtominer -u yourworkername.pts1 -p yourpassword

Mac OSX
=======

1) Install mac ports:

Follow instructions on 

http://www.macports.org/install.php

3) Compile the miner:

cd jhProtominer/src/jhProtominer
make -f makefile.osx

Windows with Cygwin64 (not tested)
==================================

1) Install AMD or NVidia SDK

For NVidia, follow instructions on 

http://docs.nvidia.com/cuda/cuda-getting-started-guide-for-microsoft-windows/index.html#installing-cuda-development-tools

For AMD, follow instructions on

http://developer.amd.com/tools-and-sdks/heterogeneous-computing/amd-accelerated-parallel-processing-app-sdk/

2) Add OpenCL to lib path:

NVidia:

export PATH=/cygdrive/c/PATH_TO_SDK/bin:$PATH
export LD_LIBRARY_PATH=/cygdrive/c/PATH_TO_SDK/lib:$LD_LIBRARY_PATH

(you might add these lines on ~/.bashrc so they are run at every login)

AMD:

???

3) Install gcc and g++ using the GUI installer

4) Compile the miner:

cd jhProtominer/src/jhProtominer
make