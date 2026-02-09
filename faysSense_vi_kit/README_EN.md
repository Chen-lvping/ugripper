### News / Events
* Nov 5, 2025 - Fix[scripts] BUG in ROS (V2.1.4).
* Nov 20, 2025 - Feat[lib] automatic/manual switching control for **initial exposure** and **gain** of the stereo camera(V2.2.0).Please refer to the configuration file instructions.
* November 21, 2025 - Feat[lib] stereo camera with left and right rotation function(V2.3.0). See the configuration file for details.
* December 4, 2025 - Feat[lib] Added support for 0/180-degree rotation of RGB images, fixed a bug in initializing the stereo camera (v2.4.0).Update[ROS] optimize ROS sample code.
* December 24, 2025 - Fix[lib] BUG(V2.5.0).
* December 31, 2025 - Feat[lib] dual-color module support(V2.6.1).
* January 19, 2026 - Feat[lib] callback function interface and added example programs for callback functions(V2.7.0).

### Supported Platforms
| Operating System | Architecture | Requirements             |
| :--------------- | :----------- | :----------------------- |
| Linux            | x64          | Ubuntu 20.04 |
| Linux            | Arm64        | Ubuntu 20.04 |

**Note:** 
* The current version natively supports only the `RK3588 (Arm64)` platform.
* Other Arm systems may require recompilation. Please contact customer service for support if you encounter issues.
* If the “Fays ViKit Version” printed in the sample code is lower than 2.0.0, please use the git checkout v1.0.0 command to switch to the matching version.

### Quick Start
#### Get the SDK
```
git clone https://cnb.cool/FaysTech/FaysSenseRelease/FaysSense_VI_Kit_Release.git
```
#### Install the driver
```
cd scripts
sudo ./setup.sh
./setup_ftdi_permissions.sh
```

#### Connect the Device
* Please use a USB 3.0 Type-C cable to connect the camera to the USB 3.0 port on the host computer. Note: The camera's Type-C port has a correct orientation. If the image cannot be read, try reversing the cable.
* Camera Definition: With the Type-C port facing downward and the camera facing forward, the camera on the left side is defined as the left camera (cam0), the camera on the right side as the right camera (cam1), and the middle camera as cam2. Factory calibration uses forward-facing images. If the image captured by your camera is inverted, you can modify the settings in the configuration (left_cam_rotate_180and right_cam_rotate_180).

![alt text](./doc/resource//cam_define.png)

![Wiring diagram: Take the UGREEN data cable as an example, the UGREEN logo is facing the front of the camera.](./doc/resource//usb.png)

### Example
The following instructions are executed by default in this directory (FaysSense_VI_Module_Release), and the sample code is located in the example directory
#### Compile Examples
**C++**
```
cd scripts && ./build.sh && cd ..
```
**ROS1**
```
cd scripts && ./build_ros.sh && cd ..
```
#### Camera connection check
```
cd scripts && ./connect_check.sh && cd ..
```
If the output is "The camera is connected to the USB 3.0 port", it means that the camera is connected normally. Otherwise, follow the script prompts to check the connection.

#### Modify Configuration File
```
./scripts/extract.sh
```
Copy the output port number to the corresponding field in config/fays_vikit.yaml. Note that the gravity acceleration value is changed to the gravity acceleration value of the location. 
* Configuration Notes: 
1. Toggle between automatic and manual modes for initial exposure​ and gain​ of the stereo cameras by modifying the values of the `stereo_init_exposure` and `stereo_gain_value` parameters in the `fays_vikit.yaml` configuration file.
2. If the left and right images are inverted, please adjust the values of `left_cam_rotate_180` and `right_cam_rotate_180`.
3. For VIKit Pro, modify the value of `rgb_rotate` to rotate the RGB image.
4. If you purchased a dual-color module, set `stereo_color_mode` to `1` for `BGR8` output mode or `0` for `RAW` output mode. Currently, dual-color cameras only support 1280 * 800 resolution—please update the relevant configurations (`stereo_single_cam_width` and `stereo_single_cam_height`).

#### Run Example Code (Root Privileges Required)
* Note the device permission changes

``` 
sudo chmod 777 /dev/video*
```
**C++**
This project provides a C++ example program for recording camera data.
``` 
cd scripts
./run_fays_record.sh
```
**ROS1**
This project provides two ROS example programs to demonstrate different ways of obtaining data via the SDK.
1. This example retrieves data by actively calling the SDK within a loop.
```
// Please start roscore in another terminal
cd scripts
./bringup_ros.sh
```
2. This example retrieves data by registering a callback function.
```
// Please start roscore in another terminal
cd scripts
./bringup_ros_callback.sh
```
* The image should appear on the screen approximately 3 seconds after the program starts.

![Result Example](./doc/resource//example.png)

**Exit the program**
Type q/Q in the running terminal and press Enter to exit the program.

**Note**

```
/**
 * Parameter range:
 * - Stereo Exposure: 1.0 ~ 507.0
 * - Stereo Gain: 1.0 ~ 15.0
 * - Stereo Resolution: 640*400*2 / 1280*800*2
 * - Stereo FPS: 1 ~ 70[640*400*2] / 1 ~ 45[1280*800*2]
 * - Rgb Exposure: 8.0 ~ 2146.0
 * - Rgb Gain: 1.0 ~ 72.0
 *
 * Note:
 * Currently, setting camera parameters are only supported with root privileges.
 */
```