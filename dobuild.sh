#!/bin/bash
cd /g/CMakePJ/MassRTS/build || exit 9
cmake --build . --config Release --target MassRTS_GPU > /g/CMakePJ/MassRTS/build/blog.txt 2>&1
echo "BUILD_RC=$?"
