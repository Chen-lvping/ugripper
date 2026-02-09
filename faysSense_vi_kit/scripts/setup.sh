#!/bin/bash

# Libraries are now in PROJECT_ROOT/libs/ instead of thirdparty/
# Get script directory and find project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [ ! -f /usr/lib/libft602.so ] && [ -d "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)" ]; then
    cp "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)/libft602.so" /usr/lib/
    cp "$PROJECT_ROOT/libs/ft602-linux-$(uname -m)/libft602.so.1.0.17" /usr/lib/
fi