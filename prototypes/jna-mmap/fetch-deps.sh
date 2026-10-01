#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/deps"
fetch() {
  local url="$1" file="$2"
  if [ ! -f "$file" ]; then
    curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$file.part"
    mv "$file.part" "$file"
  fi
}
fetch https://repo.maven.apache.org/maven2/net/java/dev/jna/jna/5.8.0/jna-5.8.0.jar jna-5.8.0.jar
fetch https://repo.maven.apache.org/maven2/org/ow2/asm/asm/9.7.1/asm-9.7.1.jar asm-9.7.1.jar
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum -c SHA256SUMS
else
  shasum -a 256 -c SHA256SUMS
fi
