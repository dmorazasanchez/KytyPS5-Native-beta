#!/usr/bin/env bash
# Run in the Debian minimal build container with /output mounted.
set -euo pipefail
mkdir -p /output/bin /output/lib /output/licenses
install -m755 /usr/bin/labwc /usr/bin/xdotool /output/bin/
for tool in labwc xdotool; do
    cat > "/output/$tool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
base=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# The loader path applies to this process and leaves the child's environment alone.
exec /lib64/ld-linux-x86-64.so.2 --library-path "$base/lib" "$base/bin/${0##*/}" "$@"
EOF
    chmod 755 "/output/$tool"
done
packages=(labwc xdotool)
while read -r soname library; do
    # Use the host glibc, graphics stack and logind/udev clients.
    case "$soname" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libEGL.so.*|libGL*.so.*|libgbm.so.*|libdrm.so.*|libvulkan.so.*|libsystemd.so.*|libudev.so.*)
            continue ;;
    esac
    install -m644 "$library" "/output/lib/$soname"
    package=$(dpkg-query -S "$(readlink -f "$library")" | sed -n '1s/: .*//p')
    packages+=("$package")
done < <(ldd /usr/bin/labwc /usr/bin/xdotool | awk '/=> \// {print $1, $3}' | sort -u)
printf '%s\n' "${packages[@]}" | sort -u | while read -r package; do
    dpkg-query -W -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' "$package"
    doc="/usr/share/doc/${package%%:*}/copyright"
    if [[ -r "$doc" ]]; then
        install -m644 "$doc" "/output/licenses/${package%%:*}.copyright"
    fi
done > /output/PACKAGES.tsv
cp -aL /usr/share/common-licenses /output/licenses/
