# Rotating 3D Castle (OpenGL)
A textured 3D castle rendered with modern OpenGL (3.3 core) that rotates around its vertical axis. The whole project is a single C++ source file, and every texture is generated procedurally in code, so no image assets are needed.

![Castle preview](preview.png)

## Features

- Central keep with a pyramid roof, four corner towers with conical roofs, battlemented walls, and a gatehouse with an arched door
- Island with a stone rim, a moat, a drawbridge, a stone path and pine trees
- 9 procedural textures: stone, roof tiles, door, window, flag, grass, water, planks, leaves
- Alpha-cutout textures for the arched door/windows and the swallow-tail flags
- Lighting with a directional light, hemisphere ambient, specular highlights on water and glass, and light distance fog
- Window opens centered on the monitor and the castle stays centered on screen

### Efficiency

- One vertex buffer and one index buffer (EBO) for the entire scene (~2,900 vertices, ~1,800 triangles)
- Cylinders and cones share ring vertices through indices
- Hidden faces (box bottoms, tower bottoms) are never generated
- One draw call per texture (9 total)

## Controls

| Key | Action |
| --- | --- |
| `SPACE` | Pause / resume rotation |
| `LEFT` / `RIGHT` | Slow down / speed up rotation |
| `UP` / `DOWN` | Zoom in / out |
| `ESC` | Quit |

## Requirements

- A C++11 (or newer) compiler
- GPU/driver supporting OpenGL 3.3
- [GLFW 3](https://www.glfw.org/)
- [GLM](https://github.com/g-truc/glm)
- [GLAD v1](https://github.com/Dav1dde/glad) generated for **OpenGL 3.3, Core profile**
- Python 3.12 or 3.13 (only if you generate GLAD with the command line; the web generator needs no Python)

## Setup

### 1. Generate GLAD (all platforms)

You should end up with this folder layout next to the source file:

```
glad/include/glad/glad.h
glad/include/KHR/khrplatform.h
glad/src/glad.c
```

**Option A: command line (needs Python)**

```bash
pip install glad==0.1.36
python -m glad --generator c --profile core --api gl=3.3 --out-path glad
```

On Windows, if `pip` is not recognized, either install Python and tick **"Add python.exe to PATH"** in the installer (then reopen PowerShell), or use the `py` launcher:

```powershell
py -m pip install glad==0.1.36
py -m glad --generator c --profile core --api gl=3.3 --out-path glad
```

Check your install with `python --version` and `pip --version`. Python 3.13 is the safest choice; this old GLAD release has only been tested on 3.12.

**Option B: web generator (no Python needed)**

1. Open <https://glad.dav1d.de/>
2. Language: **C/C++**, Specification: **OpenGL**, API gl: **Version 3.3**, Profile: **Core**, Generate a loader: **checked**
3. Click **Generate**, download `glad.zip` and unzip it into a folder named `glad` (so the layout above matches).

### 2. Install GLFW and GLM

**Ubuntu / Debian**

```bash
sudo apt install g++ libglfw3-dev libglm-dev
```

**macOS (Homebrew)**

```bash
brew install glfw glm
```

**Windows, option 1: MSYS2 (command line, recommended)**

1. Install MSYS2 from <https://www.msys2.org/>.
2. Open the **"MSYS2 MinGW64"** shell from the Start menu (not the plain MSYS or UCRT shell) and run:

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-glfw mingw-w64-x86_64-glm
```

If the first command closes the window, reopen the MinGW64 shell and run the `pacman -S` line again.

**Windows, option 2: Visual Studio with vcpkg** (PowerShell):

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install glfw3:x64-windows glm:x64-windows
C:\vcpkg\vcpkg integrate install
```

### 3. Build and run

**Linux**

```bash
g++ -std=c++17 -Iglad/include Castle.cpp glad/src/glad.c -o castle -lglfw -ldl -lGL
./castle
```

**macOS**

```bash
g++ -std=c++17 -I/opt/homebrew/include -Iglad/include Castle.cpp glad/src/glad.c \
    -o castle -L/opt/homebrew/lib -lglfw -framework OpenGL
./castle
```

(On Intel Macs Homebrew lives in `/usr/local` instead of `/opt/homebrew`.)

**Windows, MSYS2 MinGW64 shell**

```bash
cd /c/Users/<you>/Documents/castle
g++ -std=c++17 -Iglad/include Castle.cpp glad/src/glad.c -o castle.exe -lglfw3 -lopengl32 -lgdi32
./castle.exe
```

Replace `<you>` and the folder with the path to your project. Note that `C:\` becomes `/c/` in this shell.

**Windows, Visual Studio**

1. Create a new empty C++ Console App.
2. Add `Castle.cpp` and `glad\src\glad.c` to the project.
3. Right-click the project, then Properties, then C/C++, then General, and add `glad\include` to **Additional Include Directories**.
4. Properties, then Linker, then Input, and add `opengl32.lib` to **Additional Dependencies**.
5. Set the configuration to **x64** and press **Ctrl+F5**.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| `pip` is not recognized (Windows) | Install Python with "Add python.exe to PATH" ticked, then reopen PowerShell. Or try `py -m pip ...`, or use the web generator (Setup, step 1, option B). |
| `glad/glad.h: No such file` | The `-Iglad/include` path is wrong or GLAD was not generated. |
| Missing `glfw3.dll` (Windows) | Link with `-lglfw3` as shown, or add `C:\msys64\mingw64\bin` to your PATH. |
| Window opens then closes / "Failed to create window" | Your GPU or driver doesn't expose OpenGL 3.3 core. Update your graphics drivers. |
| Black or blank screen in a virtual machine | Enable 3D acceleration, or run on the host machine. |

