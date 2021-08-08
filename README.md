# amp_fwupgrade

## Ampere Firmware Upgrade Tool
This tool enables upgrading firmware on Ampere's platform.

Image input is raw image signed by dbu key.

### Compile

```
make clean static

Output: src/amp_fwupgrade-static
```

### Cross Compile

```
make CROSS_COMPILE=${CROSS_COMPILE} clean static
```

### Usage

```
amp_fwupgrade [OPTION...]
  -a, --allfw=<file>             Upgrade full firmware (excluding SCP) from <file>
                                    Ex: jade_aptiov_atf_<VERSION>.dbu.sig.img
  -c, --ueficfg=<file>         Upgrade only UEFI and board settings from <file>
                                    Ex: jade_aptiovcfg_<VERSION>.dbu.sig.img
  -u, --uefi=<file>               Upgrade only UEFI from <file>
                                    Ex: jade_aptiov_<VERSION>.dbu.sig.img
  -s, --scp=<file>                Upgrade SCP from <file>
                                    Ex: altra_scp_<VERSION>.dbu.sig.img
  [-F/-f/-C] <file>               Upgrade firmware from single <file> with the following options
        , --fullfw=<file>           -F: Full flash
        , --atfuefi=<file>          -f: Only ATF and UEFI be flashed
        , --clear=<file>            -C: Only erase FW setting
Help options:
  -?, --help                      Show this help message
      --usage                     Display brief usage message
      --version                   Display version and copyright information

```

## Tools and libraries to manipulate EFI variables

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; version 2.1
of the License.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this library.  If not, see [http://www.gnu.org/licenses/].

There is an ABI tracker for this project at [ABI Laboratory].

[http://www.gnu.org/licenses/]: http://www.gnu.org/licenses/
[ABI Laboratory]: https://abi-laboratory.pro/tracker/timeline/efivar/

# WARNING
You should probably not run "make a brick" *ever*, unless you're already
reasonably sure it won't permanently corrupt your firmware.  This is not a
joke.
