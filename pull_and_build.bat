taskkill /f /im test_renderer.exe 2>nul
taskkill /f /im deno_renderer.exe 2>nul
taskkill /f /im presenter.exe 2>nul

git pull
cmake -B build -DCMAKE_TOOLCHAIN_FILE="C:\Users\mattc\source\repos\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Debug

REM build\presenter\Debug\presenter.exe --deno

REM or:

REM build\presenter\Debug\presenter.exe --deno --no-spawn
REM deno_renderer\target\debug\deno_renderer.exe
