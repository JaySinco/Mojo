#!/bin/bash

source "./source_in_bash_profile.sh"
cwd=`readlink -f .`
fmt="${cwd}/external/.bin/clang-format.exe"
cloc="${cwd}/external/.bin/cloc.exe"

case $1 in
"")
    target=""
    ${cloc} --quiet src lib
    ;;
clean)
    rm -rf out/ bin/
    exit 0
    ;;
*)
    target="-t:$1"
    ;;
esac

BUILD_TYPE=`echo ${MSVC_BUILD_TYPE} | tr '[:lower:]' '[:upper:]'`

yarn run eslint ./lib/ --fix \
&& \
yarn run webpack \
&& \
find "${cwd}/src" -type f -exec ${fmt} -i {} \; \
&& \
mkdir -p dest/ bin/ \
&& \
cd dest/ \
&& \
${CMAKE_CMD} "${CMAKE_GENERATOR}" ../ \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_${BUILD_TYPE}=${cwd}/bin/ \
&& \
${MSVC_BUILD} ${target}
