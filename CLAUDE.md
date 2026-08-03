# OpenGL ES 2.0 glReadPixels benchmark

This is a benchmark for measuring the speed of cropped glReadPixels on Linux
using only features that are available in OpenGL ES 2.0 with the extensions that
are implemented locally.

## Architecture

 * Language: C
 * Build: meson
 * Dependencies:
   - glesv2
   - epoxy
   - json-c
   - libturbojpeg

## Method

Count how many frames can be rendered and captured within the test duration that
shall be 1 second by default.

 1. Render a rotating triangle
 2. Copy the rendered frame to a buffer allocated using malloc.
 3. Encode the frame to mjpeg via turbojpeg (appended jpeg images).

The second step is what we're testing. The whole test should be run using one of
of these methods:

 * GPU-side staging buffer
 * CPU-side staging buffer
 * Copying each row individially

All tests should be run and the number of frames captured recorded and presented
as json to stdout. Anything else, such as logs, should go to stderr.

The third step should be turned on using an option. It is only for verification.
The memcpy to the CPU allocated buffer must not be optimised out. The output
file shall be named according the the test combination.

### Defaults

 * Frame size: 1920x1080
 * Cropping region: 1280x720 in frame centre
 * Frame rate 60 frames per second

## Style

Linux Kernel Style, with exceptions for user space

/*
 * multi-line comments
 * must be written
 * like this
 */
