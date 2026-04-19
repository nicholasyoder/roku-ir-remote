# Roku IR Remote

Send Roku IR remote codes to an IR transmitter via a DB9 serial port. Works with at least the Roku 2.

Provides basical up/down/select/etc control. Perfect for getting through the initial setup wizard without a Roku remote, until it connects to a network, at which point the Roku mobile app can be used as a remote.

## Physical setup

Use the following pins from a PC's DB9 serial port:

* Pin 5 - GND
* Pin 7 - RTS

Depending on your IR LED and how much range you need, you might need to use a transister to boost the signal from the raw serial port output. For me it worked fine directly powering it from the serial port using an IR LED from an old optical mouse and having the Roku within a few inches of the LED.

## Compile and Run

```
gcc -O2 -o remote remote.c
sudo ./remote
```

Note that it needs to be run as root in order to directly control the serial port.
