librsswitch
===========
librsswitch (remote socket switch) is a Raspberry Pi Linux library for controlling remote controlled power sockets.
The library emulates the encoder chip within the power socket remote control, so a separate 433 MHz sender is needed.

librsswitch is written in C and heavily inspired by [rcswitch-pi](https://github.com/r10r/rcswitch-pi), which is based
on [rc-switch](http://code.google.com/p/rc-switch/).

Supported devices
-------
At the moment librsswitch supports the following devices:

<table>
  <tr>
    <th>Vendor ID (librsswitch)</th>
    <th>Vendor</th>
    <th>Order number</th>
    <th>Remote control encoder</th>
  </tr>
  <tr>
    <td>0</th>
    <td>REV</td>
    <td>008345</td>
    <td>PT2260-R4S</td>
  </tr>
  <tr>
    <td>1</th>
    <td>Pollin</td>
    <td>2605</td>
    <td>PT2262 (HX2262)</td>
  </tr>
</table>

If in doubt, open the remote control of your device and check which encoder is used.

Usage
-------
- Install the C library [bcm2835](http://www.open.com.au/mikem/bcm2835/) on your Raspberry Pi.
- Add a 433 MHz sender to GPIO 4 of the Raspberry Pi (see the Wiki for a detailed description).

### API
A socket is switched on or off by function socket_send().
```c
socket_send(uint dev, uint group, uint socket, uint data);
```
- **dev**: Vendor ID (check the table of supported devices)
- **group**: Socket group
- **socket**: Socket within a socket group
- **data**: 1 = On, 0 = Off
