#!/bin/zsh
# Print the XIAO's console port. Fails loudly if there is none or more than one, so a
# script never flashes the wrong board (the S3 sibling also enumerates as usbmodem*).
set -u
ports=(/dev/cu.usbmodem*(N))
case ${#ports} in
  0) echo "no /dev/cu.usbmodem* - is the XIAO plugged in? (charge-only cable?)" >&2; exit 2 ;;
  1) echo $ports[1] ;;
  *) echo "several usbmodem ports: $ports - pass the port explicitly" >&2; exit 3 ;;
esac
