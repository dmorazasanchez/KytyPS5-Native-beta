#!/bin/bash
set -euo pipefail
mkdir -p /output/lib /output/licenses
cp /usr/bin/ffmpeg /output/ffmpeg
while read -r lib; do
    name="${lib##*/}"
    case "$name" in libc.so.6|libm.so.6|ld-linux-x86-64.so.2|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2) continue ;; esac
    cp -L "$lib" "/output/lib/$name"
done < <(ldd /usr/bin/ffmpeg | awk '/=> \// {print $3}')
while read -r file; do
    package="${file%/copyright}"
    package="${package##*/}"
    cp -L "$file" "/output/licenses/$package-copyright"
done < <(find /usr/share/doc -maxdepth 2 -name copyright -type f)
dpkg-query -W > /output/PACKAGES.txt
/usr/bin/ffmpeg -version > /output/VERSION.txt
