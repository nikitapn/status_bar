# My status bar

This project is a custom status bar for X11 window systems. It displays various system information such as weather, battery status, memory usage, and the current date and time. The status bar updates periodically and shows the information in a visually appealing format using icons and colors.

## Features

- **Weather**: Displays the current temperature and weather conditions.
- **Battery**: Shows the battery level and charging status.
- **Memory**: Indicates the current memory usage.
- **Date and Time**: Displays the current date and time.

## Dependencies

To build and run this project, you need the following dependencies:

- **X11**: Xlib library for interacting with the X11 window system.
- **libudev**: Library for accessing udev device information.
- **pthread**: POSIX threads library for multithreading.
- **curl**: Library for making HTTP requests.
- **Boost**: Boost.Asio for asynchronous I/O operations.
- **nlohmann-json**: JSON library for parsing weather data.
- **fmt**: Formatting library for creating formatted strings.

You can install the required dependencies on a system using `pacman` with the following command:
``` sh
pacman -Sy ttf-font-awesome nlohmann-json
```

### Configuration
The weather block requires the MY_LOCATION environment variable to be set with the format "latitude,longitude". For example: 
``` sh
export MY_LOCATION="37.7749,-122.4194"
```

This environment variable is used to fetch the weather data for the specified location.

### License
This project is licensed under the MIT License. See the LICENSE file for details.
