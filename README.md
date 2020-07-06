# s32k-uavcanv1

S32K NuttX example to create a UAVCAN v1 publisher using libcanard

# Getting started

- Setup NuttX environment 
- Clone this project in to NuttX apps root folder
- Checkout submodules
- Install nunavut using: pip3 install nunavut
- Generate dsdl using generate_dsdl.sh
- Run make menuconfig in NuttX and enable "S32K libcanard v1 example"
- Compile NuttX

On S32K board
- Flash nuttx.bin
- Enable can0 using "ifup can0"
- start this example using "canardv1"
