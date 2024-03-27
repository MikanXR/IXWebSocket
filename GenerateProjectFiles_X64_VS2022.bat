@echo on

IF NOT EXIST build mkdir build
pushd build

echo "Rebuilding IxWebsocket Project files..."

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_UNITY_BUILD=ON -DUSE_ZLIB=OFF

IF %ERRORLEVEL% NEQ 0 (
  echo "Error generating IxWebsocket project files"
  goto failure
)

popd
EXIT \B 0

:failure
pause
EXIT \B 1